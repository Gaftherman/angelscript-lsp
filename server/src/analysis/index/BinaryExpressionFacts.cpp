#include "analysis/index/BinaryExpressionFacts.h"

#include "analysis/index/BinaryExpressionSyntax.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

static BinaryOperatorClass ClassifyBinaryOperator(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    if (sym == gs.tokIs || sym == gs.tokBangIs)
    {
        return BinaryOperatorClass::Identity;
    }
    if (sym == gs.tokEqEq || sym == gs.tokBangEq)
    {
        return BinaryOperatorClass::Equality;
    }
    if (sym == gs.tokLt || sym == gs.tokLtEq || sym == gs.tokGt || sym == gs.tokGtEq)
    {
        return BinaryOperatorClass::Relational;
    }
    if (sym == gs.tokPlus || sym == gs.tokDash || sym == gs.tokStar || sym == gs.tokSlash || sym == gs.tokPercent ||
        sym == gs.tokStarStar)
    {
        return BinaryOperatorClass::Arithmetic;
    }
    if (sym == gs.tokPipe || sym == gs.tokAmp || sym == gs.tokCaret || sym == gs.tokLtLt || sym == gs.tokGtGt ||
        sym == gs.tokGtGtGt)
    {
        return BinaryOperatorClass::Bitwise;
    }
    return BinaryOperatorClass::Other;
}

static std::pair<std::string_view, uint32_t> ExtractFoldedIdentifier(TSNode node, const NodeIndex* index,
                                                                     std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode current = node;
    while (!ts_node_is_null(current) && ts_node_symbol(current) == gs.symParenthesizedExpression)
    {
        current = SyntaxOperands(current, index).FirstNamed();
    }
    if (ts_node_is_null(current))
    {
        return {};
    }
    const TSSymbol sym = ts_node_symbol(current);
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        return {parser::NodeText(current, source), ts_node_start_byte(current)};
    }
    return {};
}

static BinaryOperandFact BuildOperandFact(TSNode node, const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_is_null(node) ? 0 : ts_node_symbol(node);
    const TSPoint startPt = ts_node_start_point(node);
    const TSPoint endPt = ts_node_end_point(node);
    const auto [foldedName, foldedByte] = ExtractFoldedIdentifier(node, &index, source);

    return BinaryOperandFact{.bytes = {ts_node_start_byte(node), ts_node_end_byte(node)},
                             .range = {startPt.row, startPt.column, endPt.row, endPt.column},
                             .text = parser::NodeText(node, source),
                             .foldedName = foldedName,
                             .foldedByte = foldedByte,
                             .isNullLiteral = IsNullInitializer(node, &index),
                             .isThis = (sym == gs.symThisExpression),
                             .hasHandleOperator = SyntaxOperands(node, &index).HasUnaryOperator(gs.tokAt),
                             .isExtractableCandidate = (sym == gs.symMemberExpression || sym == gs.symIdentifier ||
                                                        sym == gs.symCallExpression || sym == gs.symIndexExpression ||
                                                        sym == gs.symScopedIdentifier),
                             .isNumericLiteral = IsNumericLiteralExpression(node, &index)};
}

/**
 * @brief Finds the binary expression that spans exactly the given byte range.
 * @param[in] bytes Byte span of the expression to locate.
 * @return Pointer to the matching fact, or nullptr when no exact match exists.
 */
const BinaryExpressionFact* BinaryExpressionFacts::Find(IndexedByteRange bytes) const noexcept
{
    const auto it = std::lower_bound(expressions.begin(), expressions.end(), bytes,
                                     [](const BinaryExpressionFact& fact, const IndexedByteRange& target)
                                     {
                                         return fact.bytes.startByte != target.startByte
                                                    ? fact.bytes.startByte < target.startByte
                                                    : fact.bytes.endByte > target.endByte;
                                     });
    if (it != expressions.end() && it->bytes.startByte == bytes.startByte && it->bytes.endByte == bytes.endByte)
    {
        return &(*it);
    }
    return nullptr;
}

/**
 * @brief Finds the ladder rooted at the if statement that spans exactly the given byte range.
 * @param[in] rootIf Byte span of the root if statement to locate.
 * @return Pointer to the matching ladder fact, or nullptr when no exact match exists.
 */
const IfLadderFact* BinaryExpressionFacts::FindLadder(IndexedByteRange rootIf) const noexcept
{
    const auto it = std::lower_bound(ladders.begin(), ladders.end(), rootIf,
                                     [](const IfLadderFact& fact, const IndexedByteRange& target)
                                     {
                                         return fact.rootIf.startByte != target.startByte
                                                    ? fact.rootIf.startByte < target.startByte
                                                    : fact.rootIf.endByte < target.endByte;
                                     });
    if (it != ladders.end() && it->rootIf.startByte == rootIf.startByte && it->rootIf.endByte == rootIf.endByte)
    {
        return &(*it);
    }
    return nullptr;
}

/**
 * @brief Finds all binary expressions whose byte span is contained within the given range.
 * @param[in] bytes Enclosing byte range to search within.
 * @return Pointers to matching binary expressions in source order.
 */
std::vector<const BinaryExpressionFact*> BinaryExpressionFacts::FindWithin(IndexedByteRange bytes) const
{
    std::vector<const BinaryExpressionFact*> results;
    auto it =
        std::lower_bound(expressions.begin(), expressions.end(), bytes.startByte,
                         [](const BinaryExpressionFact& fact, uint32_t start) { return fact.bytes.startByte < start; });
    for (; it != expressions.end() && it->bytes.startByte < bytes.endByte; ++it)
    {
        if (it->bytes.endByte <= bytes.endByte)
        {
            results.push_back(&(*it));
        }
    }
    return results;
}

/**
 * @brief Extracts every complete binary expression and every if ladder in the document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text buffer for string views.
 * @return Populated binary expression and ladder facts.
 */
BinaryExpressionFacts BuildBinaryExpressionFacts(const NodeIndex& index, std::string_view source)
{
    BinaryExpressionFacts facts;
    const auto& gs = parser::GetGrammarSymbols();

    for (const TSNode node : index.Nodes(gs.symBinaryExpression))
    {
        const BinaryExpressionOperands operands = ReadBinaryOperands(node, &index);
        if (!operands.Complete())
        {
            continue;
        }

        const TSPoint opStart = ts_node_start_point(operands.op);
        const TSPoint opEnd = ts_node_end_point(operands.op);
        facts.expressions.push_back(
            BinaryExpressionFact{.bytes = {ts_node_start_byte(node), ts_node_end_byte(node)},
                                 .operatorRange = {opStart.row, opStart.column, opEnd.row, opEnd.column},
                                 .op = parser::OperatorSpelling(operands.op),
                                 .opClass = ClassifyBinaryOperator(ts_node_symbol(operands.op), gs),
                                 .left = BuildOperandFact(operands.left, index, source),
                                 .right = BuildOperandFact(operands.right, index, source)});
    }

    std::ranges::sort(facts.expressions,
                      [](const BinaryExpressionFact& a, const BinaryExpressionFact& b)
                      {
                          return a.bytes.startByte != b.bytes.startByte ? a.bytes.startByte < b.bytes.startByte
                                                                        : a.bytes.endByte > b.bytes.endByte;
                      });

    for (const TSNode ifNode : index.Nodes(gs.symIfStatement))
    {
        const auto* syntax = index.FindOperandSyntax(ifNode);
        if (syntax && syntax->parentSymbol == gs.symIfStatement)
        {
            continue;
        }

        const uint32_t condBegin = static_cast<uint32_t>(facts.ladderConditions.size());
        for (const TSNode cond : CollectIfLadderConditions(ifNode, &index))
        {
            facts.ladderConditions.push_back(IndexedByteRange{ts_node_start_byte(cond), ts_node_end_byte(cond)});
        }
        const uint32_t condCount = static_cast<uint32_t>(facts.ladderConditions.size() - condBegin);
        const IndexedByteRange rootSpan{ts_node_start_byte(ifNode), ts_node_end_byte(ifNode)};
        facts.ladders.push_back(IfLadderFact{rootSpan, condBegin, condCount});
    }

    std::ranges::sort(facts.ladders,
                      [](const IfLadderFact& a, const IfLadderFact& b)
                      {
                          return a.rootIf.startByte != b.rootIf.startByte ? a.rootIf.startByte < b.rootIf.startByte
                                                                          : a.rootIf.endByte < b.rootIf.endByte;
                      });

    return facts;
}

} // namespace angel_lsp::analysis
