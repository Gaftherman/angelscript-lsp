#include "analysis/index/ExpressionSiteFacts.h"

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/BinaryExpressionSyntax.h"
#include "analysis/index/PropertyAccessFacts.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/ValueExpressionFacts.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

const BinarySiteFact* ExpressionSiteFacts::FindBinary(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(binaryExpressions, bytes);
}
const UnaryPostfixSiteFact* ExpressionSiteFacts::FindUnaryPostfix(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(unaryPostfixExpressions, bytes);
}
const MemberExpressionSiteFact* ExpressionSiteFacts::FindMember(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(memberExpressions, bytes);
}
const IndexExpressionSiteFact* ExpressionSiteFacts::FindIndex(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(indexExpressions, bytes);
}
const CastExpressionSiteFact* ExpressionSiteFacts::FindCast(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(castExpressions, bytes);
}
const FoldedConstantFact* ExpressionSiteFacts::FindFoldedConstant(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(foldedConstants, bytes);
}

/**
 * @brief Converts a node's start and end points into a line/column source range.
 * @param[in] node Node to measure; a null node yields an empty range.
 * @return Zero-based range covering the node's text.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const TSPoint s = ts_node_start_point(node);
    const TSPoint e = ts_node_end_point(node);
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

/**
 * @brief Checks whether an operand is written or mutated by its parent expression.
 * @param[in] fact Indexed operand syntax record.
 * @return True for the left side of an assignment and the operand of a unary or postfix expression.
 */
static bool IsWriteOrMutation(const IndexedSyntaxNode& fact) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    return (fact.parentSymbol == gs.symAssignmentExpression && fact.field == gs.fieldLeft) ||
           fact.parentSymbol == gs.symUnaryExpression || fact.parentSymbol == gs.symPostfixExpression;
}

/**
 * @brief Reads constant folding data and parenthesized spans along an expression chain.
 * @param[in] node Starting expression node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text.
 * @param[in,out] facts Expression site facts to record unwrapped spans and folded constants.
 * @return FoldedConstantFact for the given node.
 */
static FoldedConstantFact ReadFoldedConstant(TSNode node, const NodeIndex& index, std::string_view source,
                                             ExpressionSiteFacts& facts)
{
    FoldedConstantFact fact;
    if (ts_node_is_null(node))
    {
        return fact;
    }
    const IndexedByteRange span = BytesOf(node);
    for (const auto& existing : facts.foldedConstants)
    {
        if (existing.bytes.startByte == span.startByte && existing.bytes.endByte == span.endByte)
        {
            return existing;
        }
    }
    fact.bytes = span;
    fact.range = RangeOf(node);
    fact.chainBegin = static_cast<uint32_t>(facts.unwrappedSpans.size());

    const auto& gs = parser::GetGrammarSymbols();
    TSNode current = node;
    while (!ts_node_is_null(current) && ts_node_symbol(current) == gs.symParenthesizedExpression)
    {
        facts.unwrappedSpans.push_back(BytesOf(current));
        current = SyntaxOperands(current, &index).FirstNamed();
    }
    fact.chainCount = static_cast<uint32_t>(facts.unwrappedSpans.size()) - fact.chainBegin;

    if (!ts_node_is_null(current))
    {
        if (IsNumericLiteralExpression(current, &index))
        {
            fact.isNumericLiteral = true;
        }
        else if (const TSSymbol sym = ts_node_symbol(current); sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
        {
            fact.foldedName = parser::NodeText(current, source);
            fact.foldedByte = ts_node_start_byte(current);
        }
    }
    facts.foldedConstants.push_back(fact);
    return fact;
}

static TSNode FindUnaryOperator(const SyntaxOperands& syntax) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode opNode = syntax.Field(gs.fieldOperator);
    if (!ts_node_is_null(opNode))
    {
        return opNode;
    }
    TSNode found{};
    syntax.ForEachNamed(
        [&](TSNode child)
        {
            const TSSymbol sym = ts_node_symbol(child);
            if (sym == gs.tokPlusPlus || sym == gs.tokDashDash || sym == gs.tokBang || sym == gs.tokNot ||
                sym == gs.tokDash || sym == gs.tokPlus || sym == gs.tokTilde || sym == gs.tokAt)
            {
                found = child;
                return false;
            }
            return true;
        });
    return found;
}

static TSNode FindUnaryOperand(TSNode node, const NodeIndex& index)
{
    const SyntaxOperands syntax(node, &index);
    const TSNode operand = syntax.Field(parser::GetGrammarSymbols().fieldOperand);
    if (!ts_node_is_null(operand))
    {
        return operand;
    }
    const TSNode opNode = FindUnaryOperator(syntax);
    TSNode found{};
    syntax.ForEachNamed(
        [&](TSNode child)
        {
            if (!ts_node_is_null(opNode) && ts_node_eq(child, opNode))
            {
                return true;
            }
            found = child;
            return false;
        });
    return found;
}

static UnaryPostfixSiteFact ReadUnaryPostfix(TSNode node, const NodeIndex& index, std::string_view source,
                                             ExpressionSiteFacts& facts)
{
    const SyntaxOperands syntax(node, &index);
    UnaryPostfixSiteFact fact{.bytes = BytesOf(node), .range = RangeOf(node)};
    fact.isPostfix = (ts_node_symbol(node) == parser::GetGrammarSymbols().symPostfixExpression);

    const TSNode opNode = FindUnaryOperator(syntax);
    if (!ts_node_is_null(opNode))
    {
        const auto& gs = parser::GetGrammarSymbols();
        fact.op = parser::OperatorSpelling(opNode);
        fact.operatorRange = RangeOf(opNode);
        const TSSymbol opSym = ts_node_symbol(opNode);
        fact.isIncrementOrDecrement = (opSym == gs.tokPlusPlus || opSym == gs.tokDashDash);
        fact.isAddressOfHandle = (opSym == gs.tokAt);
    }

    const TSNode operandNode = FindUnaryOperand(node, index);
    if (!ts_node_is_null(operandNode))
    {
        fact.hasOperand = true;
        fact.operandBytes = BytesOf(operandNode);
        fact.operandRange = RangeOf(operandNode);
        fact.operandText = parser::NodeText(operandNode, source);
        fact.operand = BuildBinaryOperandFact(operandNode, index, source);
        fact.operandValue = ReadValueExpression(operandNode, &index, source);
        fact.foldedOperand = ReadFoldedConstant(operandNode, index, source, facts);

        const TSNode unwrapped = UnwrapIndexedParentheses(operandNode, &index);
        const auto& gs = parser::GetGrammarSymbols();
        const bool isIndex = !ts_node_is_null(unwrapped) && ts_node_symbol(unwrapped) == gs.symIndexExpression;
        fact.operandIsIndexed = isIndex;
        const TSNode target = isIndex ? SyntaxOperands(unwrapped, &index).FieldOrNamed(gs.fieldObject) : unwrapped;
        fact.operandProperty = ReadPropertyAccess(target, &index, source);
    }
    return fact;
}

static MemberExpressionSiteFact ReadMemberExpression(TSNode node, const NodeIndex& index, std::string_view source)
{
    MemberExpressionSiteFact fact{.bytes = BytesOf(node), .range = RangeOf(node)};
    if (const auto* opFact = index.FindOperandSyntax(node))
    {
        const auto& gs = parser::GetGrammarSymbols();
        fact.isCallCallee = (opFact->parentSymbol == gs.symCallExpression && opFact->field == gs.fieldFunction);
        fact.isWriteOrMutation = IsWriteOrMutation(*opFact);
    }
    fact.propertyAccess = ReadPropertyAccess(node, &index, source);
    return fact;
}

static IndexExpressionSiteFact ReadIndexExpression(TSNode node, const NodeIndex& index, std::string_view source)
{
    IndexExpressionSiteFact fact{.bytes = BytesOf(node), .range = RangeOf(node)};
    if (const auto* opFact = index.FindOperandSyntax(node))
    {
        fact.isWriteOrMutation = IsWriteOrMutation(*opFact);
    }
    const TSNode arrayNode = SyntaxOperands(node, &index).FieldOrNamed(parser::GetGrammarSymbols().fieldObject);
    fact.objectProperty = ReadPropertyAccess(arrayNode, &index, source);
    return fact;
}

static CastExpressionSiteFact ReadCastExpression(TSNode node, const NodeIndex& index, std::string_view source,
                                                 ExpressionSiteFacts& facts)
{
    const SyntaxOperands syntax(node, &index);
    CastExpressionSiteFact fact{.bytes = BytesOf(node), .range = RangeOf(node)};

    if (const TSNode typeNode = syntax.Field(parser::GetGrammarSymbols().fieldType); !ts_node_is_null(typeNode))
    {
        fact.targetType = parser::NodeText(typeNode, source);
    }
    const TSNode valueNode = syntax.Field(parser::GetGrammarSymbols().fieldValue);
    if (!ts_node_is_null(valueNode))
    {
        fact.hasValue = true;
        fact.valueBytes = BytesOf(valueNode);
        fact.valueRange = RangeOf(valueNode);
        fact.value = ReadValueExpression(valueNode, &index, source);
        fact.valueOperand = BuildBinaryOperandFact(valueNode, index, source);
        fact.foldedValue = ReadFoldedConstant(valueNode, index, source, facts);
    }
    return fact;
}

ExpressionSiteFacts BuildExpressionSiteFacts(const NodeIndex& index, std::string_view source)
{
    ExpressionSiteFacts facts;
    const auto& gs = parser::GetGrammarSymbols();

    for (const TSNode node : index.Nodes(gs.symBinaryExpression))
    {
        facts.binaryExpressions.push_back(BinarySiteFact{BytesOf(node), RangeOf(node)});
    }
    for (const TSSymbol sym : {gs.symUnaryExpression, gs.symPostfixExpression})
    {
        for (const TSNode node : index.Nodes(sym))
        {
            facts.unaryPostfixExpressions.push_back(ReadUnaryPostfix(node, index, source, facts));
        }
    }
    for (const TSNode node : index.Nodes(gs.symMemberExpression))
    {
        facts.memberExpressions.push_back(ReadMemberExpression(node, index, source));
    }
    for (const TSNode node : index.Nodes(gs.symIndexExpression))
    {
        facts.indexExpressions.push_back(ReadIndexExpression(node, index, source));
    }
    for (const TSNode node : index.Nodes(gs.symCastExpression))
    {
        facts.castExpressions.push_back(ReadCastExpression(node, index, source, facts));
    }
    for (const TSNode node : index.Nodes(gs.symParenthesizedExpression))
    {
        ReadFoldedConstant(node, index, source, facts);
    }

    SortBySpan(facts.binaryExpressions);
    SortBySpan(facts.unaryPostfixExpressions);
    SortBySpan(facts.memberExpressions);
    SortBySpan(facts.indexExpressions);
    SortBySpan(facts.castExpressions);
    SortBySpan(facts.foldedConstants);
    return facts;
}

} // namespace angel_lsp::analysis
