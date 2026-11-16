#include "analysis/index/LValueSiteFacts.h"

#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Converts a node's start and end points to a diagnostic range.
 * @param[in] node Non-null syntax node.
 * @return Line and column span of the node.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return {start.row, start.column, end.row, end.column};
}

/**
 * @brief Converts a node to its byte span.
 * @param[in] node Non-null syntax node.
 * @return Start and end byte of the node.
 */
static IndexedByteRange BytesOf(TSNode node) noexcept
{
    return {ts_node_start_byte(node), ts_node_end_byte(node)};
}

/**
 * @brief Enqueues a child node onto the candidate expansion worklist if its span strictly differs from parent.
 * @param[in] parent Enclosing AST node.
 * @param[in] child Child AST node to evaluate as an l-value candidate.
 * @param[in,out] worklist Iterative worklist of pending candidate nodes.
 */
static void EnqueueSubCandidate(TSNode parent, TSNode child, std::vector<TSNode>& worklist)
{
    if (ts_node_is_null(child))
    {
        return;
    }
    if (ts_node_start_byte(child) == ts_node_start_byte(parent) && ts_node_end_byte(child) == ts_node_end_byte(parent))
    {
        return;
    }
    worklist.push_back(child);
}

/**
 * @brief Extracts receiver object and member identifier byte spans from a member expression node.
 * @param[in] node Member expression node.
 * @param[in] index Precomputed node index of the document.
 * @return Compact receiver and member span pair.
 */
static LValueMemberCandidateFact ReadMemberParts(TSNode node, const NodeIndex& index) noexcept
{
    LValueMemberCandidateFact parts{};
    const SyntaxOperands syntax(node, &index);
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode obj = syntax.Field(gs.fieldObject);
    const TSNode mem = syntax.Field(gs.fieldMember);
    if (!ts_node_is_null(obj))
    {
        parts.objectBytes = BytesOf(obj);
    }
    if (!ts_node_is_null(mem))
    {
        parts.memberBytes = BytesOf(mem);
    }
    return parts;
}

/**
 * @brief Extracts call l-value details from a call expression node.
 * @param[in] callNode Call expression node.
 * @param[in] index Precomputed node index of the document.
 * @return Call site fact for the call node.
 */
static LValueCallSiteFact ReadCallSite(TSNode callNode, const NodeIndex& index) noexcept
{
    LValueCallSiteFact fact;
    if (ts_node_is_null(callNode))
    {
        return fact;
    }
    fact.bytes = BytesOf(callNode);
    fact.range = RangeOf(callNode);
    const TSNode funcNode = parser::GetCallCallee(callNode);
    if (ts_node_is_null(funcNode))
    {
        return fact;
    }
    fact.calleeBytes = BytesOf(funcNode);

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol funcSym = ts_node_symbol(funcNode);
    if (funcSym == gs.symMemberExpression)
    {
        fact.calleeKind = LValueCalleeKind::Member;
        const LValueMemberCandidateFact parts = ReadMemberParts(funcNode, index);
        fact.objectBytes = parts.objectBytes;
        fact.memberBytes = parts.memberBytes;
    }
    else if (funcSym == gs.symScopedIdentifier)
    {
        fact.calleeKind = LValueCalleeKind::Scoped;
    }
    else if (funcSym == gs.symIdentifier || funcSym == gs.tokFunction)
    {
        fact.calleeKind = LValueCalleeKind::Identifier;
    }
    return fact;
}

/**
 * @brief Reads a compact l-value candidate fact and queues any nested branch or dereference operands.
 * @param[in] rawNode Expression node (may be parenthesized).
 * @param[in] index Precomputed node index of the document.
 * @param[in,out] facts Target site container holding shared ternary, member, and unary pools.
 * @param[in,out] worklist Iterative worklist receiving nested branch/operand nodes queried by FindCandidate.
 * @return Compact candidate fact for rawNode.
 */
static LValueCandidateFact ReadCandidate(TSNode rawNode, const NodeIndex& index, LValueSiteFacts& facts,
                                         std::vector<TSNode>& worklist)
{
    LValueCandidateFact fact;
    if (ts_node_is_null(rawNode))
    {
        return fact;
    }
    fact.bytes = BytesOf(rawNode);
    const TSNode node = UnwrapIndexedParentheses(rawNode, &index);
    if (ts_node_is_null(node))
    {
        return fact;
    }
    fact.unwrappedBytes = BytesOf(node);
    fact.isParenthesized =
        (fact.bytes.startByte != fact.unwrappedBytes.startByte || fact.bytes.endByte != fact.unwrappedBytes.endByte);

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symTernaryExpression)
    {
        fact.kind = LValueCandidateKind::Ternary;
        const SyntaxOperands syntax(node, &index);
        TSNode cons = syntax.Field(gs.fieldConsequence);
        TSNode alt = syntax.Field(gs.fieldAlternative);
        if ((ts_node_is_null(cons) || ts_node_is_null(alt)) && ts_node_named_child_count(node) >= 3)
        {
            cons = ts_node_named_child(node, 1);
            alt = ts_node_named_child(node, 2);
        }
        const uint32_t branchBegin = static_cast<uint32_t>(facts.ternaryBranches.size());
        if (!ts_node_is_null(cons))
        {
            facts.ternaryBranches.push_back(LValueBranchFact{BytesOf(cons), true});
            EnqueueSubCandidate(node, cons, worklist);
        }
        if (!ts_node_is_null(alt))
        {
            facts.ternaryBranches.push_back(LValueBranchFact{BytesOf(alt), false});
            EnqueueSubCandidate(node, alt, worklist);
        }
        fact.detailIndex = branchBegin;
        fact.detailCount = static_cast<uint16_t>(facts.ternaryBranches.size() - branchBegin);
    }
    else if (sym == gs.symUnaryExpression)
    {
        const SyntaxOperands syntax(node, &index);
        const TSNode op = syntax.Field(gs.fieldOperator);
        if (!ts_node_is_null(op) && (ts_node_symbol(op) == gs.tokAt || parser::NodeText(op, facts.source) == "@"))
        {
            fact.kind = LValueCandidateKind::UnaryDereference;
            const TSNode operand = syntax.Field(gs.fieldOperand);
            if (!ts_node_is_null(operand))
            {
                fact.detailIndex = static_cast<uint32_t>(facts.unaryOperands.size());
                fact.detailCount = 1;
                facts.unaryOperands.push_back(BytesOf(operand));
                EnqueueSubCandidate(node, operand, worklist);
            }
        }
    }
    else if (sym == gs.symMemberExpression)
    {
        fact.kind = LValueCandidateKind::Member;
        fact.detailIndex = static_cast<uint32_t>(facts.members.size());
        fact.detailCount = 1;
        facts.members.push_back(ReadMemberParts(node, index));
    }
    else if (sym == gs.symIndexExpression)
    {
        fact.kind = LValueCandidateKind::Index;
    }
    else if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier || sym == gs.tokFunction ||
             sym == gs.symThisExpression)
    {
        fact.kind = LValueCandidateKind::Identifier;
    }
    else if (sym == gs.symCallExpression)
    {
        fact.kind = LValueCandidateKind::Call;
    }
    else
    {
        fact.kind = LValueCandidateKind::Other;
    }
    return fact;
}

/**
 * @brief Collects an assignment expression's left-hand target and optional call-lvalue site.
 * @param[in] node Assignment expression node.
 * @param[in] index Precomputed node index of the document.
 * @param[in,out] facts Destination l-value site facts.
 * @param[in,out] worklist Iterative worklist receiving nested branch/operand nodes.
 */
static void CollectAssignmentTarget(TSNode node, const NodeIndex& index, LValueSiteFacts& facts,
                                    std::vector<TSNode>& worklist)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode target = SyntaxOperands(node, &index).Field(gs.fieldLeft);
    if (ts_node_is_null(target))
    {
        return;
    }

    const TSNode unwrapped = UnwrapIndexedParentheses(target, &index);
    LValueAssignmentFact fact;
    fact.bytes = BytesOf(node);
    fact.range = RangeOf(node);
    fact.targetRange = RangeOf(target);
    fact.targetIsDirectCall = (ts_node_symbol(target) == gs.symCallExpression);
    fact.targetIsUnwrappedCall = (!ts_node_is_null(unwrapped) && ts_node_symbol(unwrapped) == gs.symCallExpression);
    fact.targetIsDirectTernary = (ts_node_symbol(target) == gs.symTernaryExpression);
    fact.targetCandidate = ReadCandidate(target, index, facts, worklist);

    if (fact.targetIsDirectCall || fact.targetIsUnwrappedCall)
    {
        const TSNode callNode = fact.targetIsDirectCall ? target : unwrapped;
        facts.callSites.push_back(ReadCallSite(callNode, index));
    }
    facts.assignments.push_back(fact);
}

/**
 * @brief Collects an increment or decrement operator's operand target when it is not a call expression.
 * @param[in] node Unary or postfix expression node.
 * @param[in] index Precomputed node index of the document.
 * @param[in,out] facts Destination l-value site facts.
 * @param[in,out] worklist Iterative worklist receiving nested branch/operand nodes.
 */
static void CollectIncDecTargets(TSNode node, const NodeIndex& index, LValueSiteFacts& facts,
                                 std::vector<TSNode>& worklist)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, &index);
    const TSNode op = syntax.Field(gs.fieldOperator);
    if (ts_node_is_null(op))
    {
        return;
    }
    const std::string_view opText = parser::NodeText(op, facts.source);
    if (opText != "++" && opText != "--")
    {
        return;
    }
    const TSNode target = syntax.Field(gs.fieldOperand);
    if (ts_node_is_null(target))
    {
        return;
    }
    const TSNode unwrapped = UnwrapIndexedParentheses(target, &index);
    if (ts_node_is_null(unwrapped) || ts_node_symbol(unwrapped) == gs.symCallExpression)
    {
        return;
    }

    LValueAssignmentFact fact;
    fact.bytes = BytesOf(node);
    fact.range = RangeOf(node);
    fact.targetRange = RangeOf(target);
    fact.targetIsDirectTernary = (ts_node_symbol(target) == gs.symTernaryExpression);
    fact.targetCandidate = ReadCandidate(target, index, facts, worklist);
    facts.assignments.push_back(fact);
}

LValueSiteFacts BuildLValueSiteFacts(const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    LValueSiteFacts facts;
    facts.source = source;

    const auto assignNodes = index.Nodes(gs.symAssignmentExpression);
    const auto unaryNodes = index.Nodes(gs.symUnaryExpression);
    const auto postfixNodes = index.Nodes(gs.symPostfixExpression);

    facts.assignments.reserve(assignNodes.size() + unaryNodes.size() + postfixNodes.size());
    facts.callSites.reserve(assignNodes.size());

    std::vector<TSNode> worklist;
    for (const TSNode node : assignNodes)
    {
        CollectAssignmentTarget(node, index, facts, worklist);
    }
    for (const TSNode node : unaryNodes)
    {
        CollectIncDecTargets(node, index, facts, worklist);
    }
    for (const TSNode node : postfixNodes)
    {
        CollectIncDecTargets(node, index, facts, worklist);
    }

    if (!facts.assignments.empty())
    {
        const size_t maxSubCandidates =
            worklist.empty() ? 0 : (index.Nodes(gs.symTernaryExpression).size() * 2 + unaryNodes.size());
        facts.candidates.reserve(facts.assignments.size() + maxSubCandidates);
        for (const auto& assignment : facts.assignments)
        {
            facts.candidates.push_back(assignment.targetCandidate);
        }
    }

    while (!worklist.empty())
    {
        const TSNode workNode = worklist.back();
        worklist.pop_back();
        facts.candidates.push_back(ReadCandidate(workNode, index, facts, worklist));
    }

    SortBySpan(facts.assignments);
    SortBySpan(facts.callSites);
    SortBySpan(facts.candidates);

    const auto sameSpan = [](const auto& a, const auto& b) noexcept
    { return a.bytes.startByte == b.bytes.startByte && a.bytes.endByte == b.bytes.endByte; };

    facts.callSites.erase(std::unique(facts.callSites.begin(), facts.callSites.end(), sameSpan), facts.callSites.end());
    facts.candidates.erase(std::unique(facts.candidates.begin(), facts.candidates.end(), sameSpan),
                           facts.candidates.end());

    return facts;
}

} // namespace angel_lsp::analysis
