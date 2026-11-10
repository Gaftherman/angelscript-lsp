#include "analysis/index/LValueSiteFacts.h"

#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/SemanticHelpers.h"
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
 * @brief Reads receiver object and member spans and text from a member expression.
 * @param[in] node Member expression node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text.
 * @return LValueMemberParts containing receiver and member information.
 */
static LValueMemberParts ReadMemberParts(TSNode node, const NodeIndex& index, std::string_view source)
{
    LValueMemberParts parts{};
    const SyntaxOperands syntax(node, &index);
    const TSNode obj = syntax.Field(parser::GetGrammarSymbols().fieldObject);
    const TSNode mem = syntax.Field(parser::GetGrammarSymbols().fieldMember);
    if (!ts_node_is_null(obj))
    {
        parts.objBytes = BytesOf(obj);
        parts.objRange = RangeOf(obj);
        parts.objText = parser::NodeText(obj, source);
    }
    if (!ts_node_is_null(mem))
    {
        parts.memBytes = BytesOf(mem);
        parts.memRange = RangeOf(mem);
        parts.memText = parser::NodeText(mem, source);
    }
    return parts;
}

/**
 * @brief Extracts call l-value details from a call expression node.
 * @param[in] callNode Call expression node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text.
 * @return Call site fact for the call node.
 */
static LValueCallSiteFact ReadCallSite(TSNode callNode, const NodeIndex& index, std::string_view source)
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
    fact.calleeRange = RangeOf(funcNode);
    fact.calleeText = parser::NodeText(funcNode, source);

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol funcSym = ts_node_symbol(funcNode);
    if (funcSym == gs.symMemberExpression)
    {
        fact.calleeKind = LValueCalleeKind::Member;
        ReadMemberParts(funcNode, index, source).ApplyTo(fact);
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
 * @brief Reads an l-value candidate fact from an expression node.
 * @param[in] rawNode Expression node (may be parenthesized).
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text.
 * @param[in,out] branches Flat buffer to append ternary branches into.
 * @return Candidate fact with flattened recursive structure.
 */
static LValueCandidateFact ReadCandidate(TSNode rawNode, const NodeIndex& index, std::string_view source,
                                         std::vector<LValueBranchFact>& branches)
{
    LValueCandidateFact fact;
    if (ts_node_is_null(rawNode))
    {
        return fact;
    }
    fact.bytes = BytesOf(rawNode);
    fact.range = RangeOf(rawNode);
    const TSNode node = UnwrapIndexedParentheses(rawNode, &index);
    if (ts_node_is_null(node))
    {
        return fact;
    }
    fact.unwrappedBytes = BytesOf(node);
    fact.unwrappedRange = RangeOf(node);
    fact.text = parser::NodeText(node, source);
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
        fact.branchBegin = static_cast<uint32_t>(branches.size());
        if (!ts_node_is_null(cons))
        {
            fact.consequenceBytes = BytesOf(cons);
            fact.consequenceRange = RangeOf(cons);
            fact.consequenceText = parser::NodeText(cons, source);
            branches.push_back(
                LValueBranchFact{fact.consequenceBytes, fact.consequenceRange, fact.consequenceText, true});
        }
        if (!ts_node_is_null(alt))
        {
            fact.alternativeBytes = BytesOf(alt);
            fact.alternativeRange = RangeOf(alt);
            fact.alternativeText = parser::NodeText(alt, source);
            branches.push_back(
                LValueBranchFact{fact.alternativeBytes, fact.alternativeRange, fact.alternativeText, false});
        }
        fact.branchCount = static_cast<uint32_t>(branches.size() - fact.branchBegin);
    }
    else if (sym == gs.symUnaryExpression)
    {
        const SyntaxOperands syntax(node, &index);
        const TSNode op = syntax.Field(gs.fieldOperator);
        if (!ts_node_is_null(op) && (ts_node_symbol(op) == gs.tokAt || parser::NodeText(op, source) == "@"))
        {
            fact.kind = LValueCandidateKind::UnaryDereference;
            const TSNode operand = syntax.Field(gs.fieldOperand);
            if (!ts_node_is_null(operand))
            {
                fact.operandBytes = BytesOf(operand);
                fact.operandRange = RangeOf(operand);
                fact.operandText = parser::NodeText(operand, source);
            }
        }
    }
    else if (sym == gs.symMemberExpression)
    {
        fact.kind = LValueCandidateKind::Member;
        ReadMemberParts(node, index, source).ApplyTo(fact);
    }
    else if (sym == gs.symIndexExpression)
    {
        fact.kind = LValueCandidateKind::Index;
    }
    else if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier || sym == gs.tokFunction)
    {
        fact.kind = LValueCandidateKind::Identifier;
        fact.name = fact.text;
        fact.refByte = ts_node_start_byte(node);
        fact.isBareDataType = IsPrimitiveTypeName(fact.text);
    }
    else if (sym == gs.symCallExpression)
    {
        fact.kind = LValueCandidateKind::Call;
        fact.isCall = true;
        fact.callBytes = fact.unwrappedBytes;
    }
    else
    {
        fact.kind = LValueCandidateKind::Other;
        fact.isBareDataType = (sym == gs.symPrimitiveType || IsPrimitiveTypeName(fact.text));
    }
    return fact;
}

/**
 * @brief Reads an assignment expression into an assignment fact.
 * @param[in] node Assignment expression node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text.
 * @param[in,out] branches Flat buffer to append ternary branches into.
 * @return Assignment fact for the node.
 */
static LValueAssignmentFact ReadAssignment(TSNode node, const NodeIndex& index, std::string_view source,
                                           std::vector<LValueBranchFact>& branches)
{
    LValueAssignmentFact fact;
    fact.bytes = BytesOf(node);
    fact.range = RangeOf(node);
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode target = SyntaxOperands(node, &index).Field(gs.fieldLeft);
    if (ts_node_is_null(target))
    {
        return fact;
    }
    const TSNode unwrapped = UnwrapIndexedParentheses(target, &index);
    fact.targetBytes = BytesOf(target);
    fact.targetRange = RangeOf(target);
    fact.targetText = parser::NodeText(target, source);
    fact.targetIsDirectCall = (ts_node_symbol(target) == gs.symCallExpression);
    fact.targetIsUnwrappedCall = (!ts_node_is_null(unwrapped) && ts_node_symbol(unwrapped) == gs.symCallExpression);
    fact.callBytes = fact.targetIsDirectCall ? fact.targetBytes : BytesOf(unwrapped);
    fact.targetIsDirectTernary = (ts_node_symbol(target) == gs.symTernaryExpression);
    fact.targetCandidate = ReadCandidate(target, index, source, branches);
    return fact;
}

LValueSiteFacts BuildLValueSiteFacts(const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    LValueSiteFacts facts;

    for (const TSNode node : index.Nodes(gs.symAssignmentExpression))
    {
        const TSNode target = SyntaxOperands(node, &index).Field(gs.fieldLeft);
        if (ts_node_is_null(target))
        {
            continue;
        }

        const LValueAssignmentFact assignFact = ReadAssignment(node, index, source, facts.ternaryBranches);
        facts.assignments.push_back(assignFact);

        if (assignFact.targetIsDirectCall || assignFact.targetIsUnwrappedCall)
        {
            const TSNode callNode = assignFact.targetIsDirectCall ? target : UnwrapIndexedParentheses(target, &index);
            facts.callSites.push_back(ReadCallSite(callNode, index, source));
        }
    }

    const TSSymbol candidateSymbols[] = {
        gs.symIdentifier,        gs.symScopedIdentifier, gs.tokFunction,
        gs.symMemberExpression,  gs.symIndexExpression,  gs.symUnaryExpression,
        gs.symTernaryExpression, gs.symCallExpression,   gs.symParenthesizedExpression};

    for (const TSSymbol sym : candidateSymbols)
    {
        for (const TSNode node : index.Nodes(sym))
        {
            facts.candidates.push_back(ReadCandidate(node, index, source, facts.ternaryBranches));
        }
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
