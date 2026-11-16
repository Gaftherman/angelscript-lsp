#include "analysis/index/ExpressionSiteFacts.h"

#include "analysis/index/BinaryExpressionSyntax.h"
#include "analysis/index/PropertyAccessFacts.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/ValueExpressionFacts.h"
#include "parser/GrammarSymbols.h"
#include "utils/PositionEncoding.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

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
 * @brief Maps a Tree-Sitter unary or postfix operator token symbol to UnaryPostfixOp.
 * @param[in] sym Tree-Sitter symbol of the operator token.
 * @return Compact enum value, or UnaryPostfixOp::None when unrecognized.
 */
static UnaryPostfixOp ClassifyUnaryOp(TSSymbol sym) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const std::pair<TSSymbol, UnaryPostfixOp> table[] = {
        {gs.tokPlusPlus, UnaryPostfixOp::PlusPlus},
        {gs.tokDashDash, UnaryPostfixOp::MinusMinus},
        {gs.tokAt, UnaryPostfixOp::At},
        {gs.tokBang, UnaryPostfixOp::Bang},
        {gs.tokNot, UnaryPostfixOp::Not},
        {gs.tokDash, UnaryPostfixOp::Minus},
        {gs.tokPlus, UnaryPostfixOp::Plus},
        {gs.tokTilde, UnaryPostfixOp::Tilde},
    };
    for (const auto [tok, op] : table)
    {
        if (sym == tok)
            return op;
    }
    return UnaryPostfixOp::None;
}

/**
 * @brief Reads constant folding data and parenthesized spans along a parenthesized expression chain.
 * @param[in] node Starting parenthesized expression node.
 * @param[in] index Precomputed node index of the document.
 * @param[in,out] facts Expression site facts to record unwrapped spans and folded constants.
 */
static void ReadFoldedConstant(TSNode node, const NodeIndex& index, ExpressionSiteFacts& facts)
{
    if (ts_node_is_null(node))
        return;
    FoldedConstantFact fact;
    fact.bytes = BytesOf(node);
    fact.chainBegin = static_cast<uint32_t>(facts.unwrappedSpans.size());

    const auto& gs = parser::GetGrammarSymbols();
    TSNode current = node;
    while (!ts_node_is_null(current) && ts_node_symbol(current) == gs.symParenthesizedExpression)
    {
        facts.unwrappedSpans.push_back(BytesOf(current));
        current = SyntaxOperands(current, &index).FirstNamed();
    }
    fact.chainCount = static_cast<uint16_t>(facts.unwrappedSpans.size() - fact.chainBegin);

    if (!ts_node_is_null(current))
    {
        if (IsNumericLiteralExpression(current, &index))
            fact.isNumericLiteral = true;
        else if (const TSSymbol sym = ts_node_symbol(current); sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
            fact.foldedNameBytes = BytesOf(current);
    }
    facts.foldedConstants.push_back(fact);
}

static TSNode FindUnaryOperator(const SyntaxOperands& syntax) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode opNode = syntax.Field(gs.fieldOperator);
    if (!ts_node_is_null(opNode))
        return opNode;
    TSNode found{};
    syntax.ForEachNamed(
        [&](TSNode child)
        {
            if (ClassifyUnaryOp(ts_node_symbol(child)) != UnaryPostfixOp::None)
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
        return operand;
    const TSNode opNode = FindUnaryOperator(syntax);
    TSNode found{};
    syntax.ForEachNamed(
        [&](TSNode child)
        {
            if (!ts_node_is_null(opNode) && ts_node_eq(child, opNode))
                return true;
            found = child;
            return false;
        });
    return found;
}

/**
 * @brief Appends a non-empty PropertyAccessFact into the shared pool and returns its index.
 * @param[in] node Expression node passed to ReadPropertyAccess.
 * @param[in] index Precomputed node index of the document.
 * @param[in,out] facts Expression site facts owning source and propertyAccesses.
 * @return Pool index, or k_noExpressionFactIndex when node does not name a property.
 */
static uint32_t StorePropertyAccess(TSNode node, const NodeIndex& index, ExpressionSiteFacts& facts)
{
    const PropertyAccessFact prop = ReadPropertyAccess(node, &index, facts.source);
    if (prop.kind == PropertyAccessKind::None)
        return k_noExpressionFactIndex;
    const uint32_t slot = static_cast<uint32_t>(facts.propertyAccesses.size());
    facts.propertyAccesses.push_back(prop);
    return slot;
}

static UnaryPostfixSiteFact ReadUnaryPostfix(TSNode node, const NodeIndex& index, ExpressionSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, &index);
    UnaryPostfixSiteFact fact{.bytes = BytesOf(node)};
    fact.isPostfix = (ts_node_symbol(node) == gs.symPostfixExpression);

    const TSNode opNode = FindUnaryOperator(syntax);
    if (!ts_node_is_null(opNode))
    {
        fact.op = ClassifyUnaryOp(ts_node_symbol(opNode));
        fact.isIncrementOrDecrement = (fact.op == UnaryPostfixOp::PlusPlus || fact.op == UnaryPostfixOp::MinusMinus);
        fact.isAddressOfHandle = (fact.op == UnaryPostfixOp::At);
    }

    const TSNode operandNode = FindUnaryOperand(node, index);
    if (!ts_node_is_null(operandNode))
    {
        fact.hasOperand = true;
        fact.operandBytes = BytesOf(operandNode);
        fact.operandIsNumber =
            (ReadValueExpression(operandNode, &index, facts.source).kind == ValueExpressionKind::Number);

        if (fact.isIncrementOrDecrement)
        {
            const TSNode unwrapped = UnwrapIndexedParentheses(operandNode, &index);
            const bool isIndex = !ts_node_is_null(unwrapped) && ts_node_symbol(unwrapped) == gs.symIndexExpression;
            fact.operandIsIndexed = isIndex;
            const TSNode target = isIndex ? SyntaxOperands(unwrapped, &index).FieldOrNamed(gs.fieldObject) : unwrapped;
            fact.operandPropertyIndex = StorePropertyAccess(target, index, facts);
        }
    }
    return fact;
}

static MemberExpressionSiteFact ReadMemberExpression(TSNode node, const NodeIndex& index, ExpressionSiteFacts& facts)
{
    MemberExpressionSiteFact fact{.bytes = BytesOf(node)};
    if (const auto* opFact = index.FindOperandSyntax(node))
    {
        const auto& gs = parser::GetGrammarSymbols();
        fact.isCallCallee = (opFact->parentSymbol == gs.symCallExpression && opFact->field == gs.fieldFunction);
        fact.isWriteOrMutation = IsWriteOrMutation(*opFact);
    }
    if (!fact.isCallCallee && !fact.isWriteOrMutation)
        fact.propertyAccessIndex = StorePropertyAccess(node, index, facts);
    return fact;
}

static IndexExpressionSiteFact ReadIndexExpression(TSNode node, const NodeIndex& index, ExpressionSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, &index);
    IndexExpressionSiteFact fact{.bytes = BytesOf(node),
                                 .hasSyntaxError = ts_node_has_error(node) || index.NodeHasSyntaxError(node)};
    if (const auto* opFact = index.FindOperandSyntax(node))
        fact.isWriteOrMutation = IsWriteOrMutation(*opFact);
    const TSNode arrayNode = syntax.FieldOrNamed(gs.fieldObject);
    if (!ts_node_is_null(arrayNode))
    {
        fact.objectBytes = BytesOf(arrayNode);
        fact.objectPropertyIndex = StorePropertyAccess(arrayNode, index, facts);
    }
    fact.argBegin = static_cast<uint32_t>(facts.indexArgs.size());
    if (const auto* parent = syntax.Fact())
    {
        for (const auto* child = index.SyntaxNode(parent->first); child; child = index.SyntaxNode(child->next))
        {
            if (child->field == gs.fieldIndex)
                facts.indexArgs.push_back(BytesOf(index.SyntaxHandle(*child)));
        }
    }
    else if (const TSNode idxNode = syntax.Field(gs.fieldIndex); !ts_node_is_null(idxNode))
    {
        facts.indexArgs.push_back(BytesOf(idxNode));
    }
    fact.argCount = static_cast<uint16_t>(facts.indexArgs.size() - fact.argBegin);
    return fact;
}

static CastExpressionSiteFact ReadCastExpression(TSNode node, const NodeIndex& index, ExpressionSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, &index);
    CastExpressionSiteFact fact{.bytes = BytesOf(node)};
    if (const TSNode typeNode = syntax.Field(gs.fieldType); !ts_node_is_null(typeNode))
        fact.targetTypeBytes = BytesOf(typeNode);
    const TSNode valueNode = syntax.Field(gs.fieldValue);
    if (!ts_node_is_null(valueNode))
    {
        fact.hasValue = true;
        fact.valueIndex = static_cast<uint32_t>(facts.castValues.size());
        facts.castValues.push_back(ReadValueExpression(valueNode, &index, facts.source));
    }
    return fact;
}

ExpressionSiteFacts BuildExpressionSiteFacts(const NodeIndex& index, std::string_view source)
{
    ExpressionSiteFacts facts;
    facts.source = source;
    facts.lineIndex = utils::LineIndex::Build(source);
    const auto& gs = parser::GetGrammarSymbols();

    const auto binaryNodes = index.Nodes(gs.symBinaryExpression);
    const auto unaryNodes = index.Nodes(gs.symUnaryExpression);
    const auto postfixNodes = index.Nodes(gs.symPostfixExpression);
    const auto memberNodes = index.Nodes(gs.symMemberExpression);
    const auto indexNodes = index.Nodes(gs.symIndexExpression);
    const auto castNodes = index.Nodes(gs.symCastExpression);
    const auto parenNodes = index.Nodes(gs.symParenthesizedExpression);

    facts.binaryExpressions.reserve(binaryNodes.size());
    facts.unaryPostfixExpressions.reserve(unaryNodes.size() + postfixNodes.size());
    facts.memberExpressions.reserve(memberNodes.size());
    facts.indexExpressions.reserve(indexNodes.size());
    facts.castExpressions.reserve(castNodes.size());
    facts.foldedConstants.reserve(parenNodes.size());
    facts.unwrappedSpans.reserve(parenNodes.size());
    facts.indexArgs.reserve(indexNodes.size());
    facts.propertyAccesses.reserve(memberNodes.size() + indexNodes.size());
    facts.castValues.reserve(castNodes.size());

    for (const TSNode node : binaryNodes)
        facts.binaryExpressions.push_back(BinarySiteFact{BytesOf(node)});
    for (const TSNode node : unaryNodes)
        facts.unaryPostfixExpressions.push_back(ReadUnaryPostfix(node, index, facts));
    for (const TSNode node : postfixNodes)
        facts.unaryPostfixExpressions.push_back(ReadUnaryPostfix(node, index, facts));
    for (const TSNode node : memberNodes)
        facts.memberExpressions.push_back(ReadMemberExpression(node, index, facts));
    for (const TSNode node : indexNodes)
        facts.indexExpressions.push_back(ReadIndexExpression(node, index, facts));
    for (const TSNode node : castNodes)
        facts.castExpressions.push_back(ReadCastExpression(node, index, facts));
    for (const TSNode node : parenNodes)
        ReadFoldedConstant(node, index, facts);

    SortBySpan(facts.binaryExpressions);
    SortBySpan(facts.unaryPostfixExpressions);
    SortBySpan(facts.memberExpressions);
    SortBySpan(facts.indexExpressions);
    SortBySpan(facts.castExpressions);
    SortBySpan(facts.foldedConstants);
    return facts;
}

} // namespace angel_lsp::analysis
