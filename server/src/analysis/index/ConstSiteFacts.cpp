#include "analysis/index/ConstSiteFacts.h"

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Converts a node's start and end points to a diagnostic range.
 * @param[in] node Syntax node.
 * @return Line and column span of the node, or a zero range if null.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
        return {};
    const TSPoint s = ts_node_start_point(node), e = ts_node_end_point(node);
    return {s.row, s.column, e.row, e.column};
}

/**
 * @brief Converts a node to its byte span.
 * @param[in] node Syntax node.
 * @return Start and end byte of the node, or a zero range if null.
 */
static IndexedByteRange BytesOf(TSNode node) noexcept
{
    return ts_node_is_null(node) ? IndexedByteRange{}
                                 : IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)};
}

static ConstChainLeafFact ResolveChainLeaf(TSNode startNode, const NodeIndex& index, std::string_view source)
{
    ConstChainLeafFact info;
    TSNode cur = startNode;
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(cur))
    {
        const TSSymbol sym = ts_node_symbol(cur);
        if (sym == gs.symMemberExpression)
        {
            info.inMemberChain = true;
            cur = SyntaxOperands(cur, &index).Field(gs.fieldObject);
        }
        else if (sym == gs.symParenthesizedExpression)
            cur = SyntaxOperands(cur, &index).FirstNamed();
        else
            break;
    }
    if (!ts_node_is_null(cur))
    {
        info.leafBytes = BytesOf(cur);
        info.leafText = parser::NodeText(cur, source);
        const TSSymbol leafSym = ts_node_symbol(cur);
        info.leafIsThis = (leafSym == gs.symThisExpression ||
                           (leafSym == gs.symIdentifier && info.leafText == parser::keywords::This));
    }
    return info;
}

static ConstTargetKind ClassifyTargetKind(TSSymbol sym, std::string_view text) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symThisExpression || (sym == gs.symIdentifier && text == parser::keywords::This))
        return ConstTargetKind::This;
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
        return ConstTargetKind::Identifier;
    return sym == gs.symIndexExpression ? ConstTargetKind::Index : ConstTargetKind::Other;
}

static ConstAssignmentFact ReadConstAssignment(TSNode node, TSNode left, const NodeIndex& index,
                                               std::string_view source)
{
    ConstAssignmentFact fact{
        BytesOf(node), RangeOf(node), {BytesOf(left), RangeOf(left), parser::NodeText(left, source)}};
    ConstAssignmentTargetFact& target = fact.target;
    const auto& gs = parser::GetGrammarSymbols();
    TSNode actual = left;
    if (ts_node_symbol(left) == gs.symUnaryExpression)
    {
        const SyntaxOperands unary(left, &index);
        const TSNode op = unary.Field(gs.fieldOperator);
        if (!ts_node_is_null(op) && ts_node_symbol(op) == gs.tokAt)
        {
            target.isHandle = true;
            const TSNode operand = unary.Field(gs.fieldOperand);
            if (!ts_node_is_null(operand))
                actual = operand;
        }
    }

    target.actualTargetBytes = BytesOf(actual);
    target.actualTargetRange = RangeOf(actual);
    target.actualTargetText = parser::NodeText(actual, source);
    target.kind = ClassifyTargetKind(ts_node_symbol(actual), target.actualTargetText);
    if (ts_node_symbol(actual) == gs.symMemberExpression)
    {
        const SyntaxOperands mem(actual, &index);
        const TSNode obj = mem.Field(gs.fieldObject);
        const TSNode member = mem.Field(gs.fieldMember);
        if (!ts_node_is_null(member))
            target.memberName = parser::NodeText(member, source);
        if (!ts_node_is_null(obj))
        {
            target.receiverBytes = BytesOf(obj);
            target.receiverText = parser::NodeText(obj, source);
            const TSSymbol objSym = ts_node_symbol(obj);
            const bool isThis = objSym == gs.symThisExpression ||
                                (objSym == gs.symIdentifier && target.receiverText == parser::keywords::This);
            target.kind = isThis ? ConstTargetKind::ThisMember : ConstTargetKind::Member;
        }
    }

    const ConstChainLeafFact leaf = ResolveChainLeaf(actual, index, source);
    target.leafBytes = leaf.leafBytes;
    target.leafText = leaf.leafText;
    target.leafIsThis = leaf.leafIsThis;
    target.inMemberChain = leaf.inMemberChain;
    return fact;
}

static ConstMethodCallFact ReadConstMethodCall(TSNode node, TSNode callee, const NodeIndex& index,
                                               std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands calleeSyntax(callee, &index);
    const TSNode obj = calleeSyntax.Field(gs.fieldObject);
    const TSNode member = calleeSyntax.Field(gs.fieldMember);
    const ConstChainLeafFact leaf = ResolveChainLeaf(obj, index, source);
    return ConstMethodCallFact{BytesOf(node),
                               RangeOf(node),
                               BytesOf(callee),
                               BuildBinaryOperandFact(obj, index, source),
                               parser::NodeText(member, source),
                               BytesOf(member),
                               RangeOf(member),
                               leaf.leafBytes,
                               leaf.leafText,
                               leaf.leafIsThis,
                               leaf.inMemberChain};
}

/**
 * @brief Reads parameter declarations from a lambda parameter list node into facts arena.
 * @param[in] paramsNode Tree-Sitter AST node for the lambda parameter list.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text buffer.
 * @param[in,out] out Destination vector for extracted parameter facts.
 */
static void ReadLambdaParameters(TSNode paramsNode, const NodeIndex& index, std::string_view source,
                                 std::vector<ConstParameterFact>& out)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode typeNode{}, nameNode{};
    uint32_t startByte = UINT32_MAX, endByte = 0;

    auto finishParam = [&]()
    {
        if (!ts_node_is_null(nameNode))
        {
            const std::string_view typeText =
                ts_node_is_null(typeNode) ? std::string_view{} : parser::NodeText(typeNode, source);
            const IndexedByteRange bytes =
                startByte <= endByte ? IndexedByteRange{startByte, endByte} : IndexedByteRange{};
            out.push_back(ConstParameterFact{bytes, parser::NodeText(nameNode, source), typeText,
                                             HasLeadingConst(typeText), HasTrailingConst(typeText)});
        }
        typeNode = {};
        nameNode = {};
        startByte = UINT32_MAX;
        endByte = 0;
    };

    SyntaxOperands(paramsNode, &index)
        .ForEachChild(
            [&](TSNode child, TSFieldId field)
            {
                const TSSymbol sym = ts_node_symbol(child);
                if (sym == gs.tokCloseParen)
                    return false;
                if (sym == gs.tokOpenParen || sym == gs.symComment)
                    return true;
                if (sym == gs.tokComma)
                    return finishParam(), true;
                startByte = std::min(startByte, static_cast<uint32_t>(ts_node_start_byte(child)));
                endByte = std::max(endByte, static_cast<uint32_t>(ts_node_end_byte(child)));
                if (field == gs.fieldParamType || sym == gs.symType)
                    typeNode = child;
                else if (field == gs.fieldName || sym == gs.symIdentifier)
                    nameNode = child;
                return true;
            });

    finishParam();
}

static void ReadParameters(TSNode paramsNode, const NodeIndex& index, std::string_view source,
                           std::vector<ConstParameterFact>& out)
{
    if (ts_node_is_null(paramsNode))
        return;
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(paramsNode) == gs.symLambdaParameterList)
        return ReadLambdaParameters(paramsNode, index, source, out);
    const uint32_t count = ts_node_named_child_count(paramsNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        const TSNode p = ts_node_named_child(paramsNode, i);
        if (ts_node_symbol(p) != gs.symParameter)
            continue;
        const SyntaxOperands s(p, &index);
        const TSNode name = s.Field(gs.fieldName), type = s.Field(gs.fieldParamType);
        if (ts_node_is_null(name) || ts_node_is_null(type))
            continue;
        const std::string_view typeText = parser::NodeText(type, source);
        out.push_back(ConstParameterFact{BytesOf(p), parser::NodeText(name, source), typeText,
                                         HasLeadingConst(typeText), HasTrailingConst(typeText)});
    }
}

static EnclosingMethodFact ReadEnclosingMethod(TSNode funcNode, const NodeIndex& index, std::string_view source,
                                               std::vector<ConstParameterFact>& outParams)
{
    EnclosingMethodFact fact{.bytes = BytesOf(funcNode), .range = RangeOf(funcNode)};
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode classNode = parser::FindAncestorWithSymbol(funcNode, gs.symClassDeclaration);
    if (!ts_node_is_null(classNode))
    {
        fact.inClass = true;
        const TSNode classNameNode = SyntaxOperands(classNode, &index).Field(gs.fieldName);
        if (!ts_node_is_null(classNameNode))
            fact.className = parser::NodeText(classNameNode, source);
    }
    const SyntaxOperands funcSyntax(funcNode, &index);
    const TSNode nameNode = funcSyntax.Field(gs.fieldName);
    if (!ts_node_is_null(nameNode))
        fact.methodName = parser::NodeText(nameNode, source);

    const TSNode attr = parser::FindChildWithSymbol(funcNode, gs.symFuncAttributes);
    fact.isConstMethod = !ts_node_is_null(parser::FindChildWithSymbol(funcNode, gs.tokConst)) ||
                         (!ts_node_is_null(attr) && !ts_node_is_null(parser::FindChildWithSymbol(attr, gs.tokConst)));

    fact.parameterBegin = static_cast<uint32_t>(outParams.size());
    ReadParameters(funcSyntax.Field(gs.fieldParameters), index, source, outParams);
    fact.parameterCount = static_cast<uint32_t>(outParams.size() - fact.parameterBegin);
    return fact;
}

ConstSiteFacts BuildConstSiteFacts(const NodeIndex& index, std::string_view source)
{
    ConstSiteFacts facts;
    const auto& gs = parser::GetGrammarSymbols();

    for (const TSNode node : index.Nodes(gs.symAssignmentExpression))
        if (const TSNode left = SyntaxOperands(node, &index).Field(gs.fieldLeft); !ts_node_is_null(left))
            facts.assignments.push_back(ReadConstAssignment(node, left, index, source));

    for (const TSNode node : index.Nodes(gs.symCallExpression))
    {
        const TSNode callee = SyntaxOperands(node, &index).Field(gs.fieldFunction);
        if (!ts_node_is_null(callee) && ts_node_symbol(callee) == gs.symMemberExpression)
        {
            const SyntaxOperands calleeSyntax(callee, &index);
            const TSNode obj = calleeSyntax.Field(gs.fieldObject);
            const TSNode mem = calleeSyntax.Field(gs.fieldMember);
            if (!ts_node_is_null(obj) && !ts_node_is_null(mem))
                facts.methodCalls.push_back(ReadConstMethodCall(node, callee, index, source));
        }
    }

    for (const TSSymbol sym : {gs.symFuncDeclaration, gs.symAccessor})
        for (const TSNode node : index.Nodes(sym))
        {
            facts.enclosingMethods.push_back(ReadEnclosingMethod(node, index, source, facts.parameters));
            const auto& last = facts.enclosingMethods.back();
            facts.parameterScopes.push_back(
                ConstParameterScopeFact{BytesOf(node), last.parameterBegin, last.parameterCount});
        }

    for (const TSSymbol sym : {gs.symLambdaExpression, gs.symVirtualProperty})
        for (const TSNode node : index.Nodes(sym))
        {
            const auto begin = static_cast<uint32_t>(facts.parameters.size());
            TSNode params = SyntaxOperands(node, &index).Field(gs.fieldParameters);
            if (ts_node_is_null(params) && sym == gs.symLambdaExpression)
                params = SyntaxOperands(node, &index).FirstNamed(gs.symLambdaParameterList);
            ReadParameters(params, index, source, facts.parameters);
            facts.parameterScopes.push_back(
                ConstParameterScopeFact{BytesOf(node), begin, static_cast<uint32_t>(facts.parameters.size() - begin)});
        }

    SortBySpan(facts.assignments);
    SortBySpan(facts.methodCalls);
    SortBySpan(facts.enclosingMethods);
    SortBySpan(facts.parameterScopes);
    return facts;
}

} // namespace angel_lsp::analysis
