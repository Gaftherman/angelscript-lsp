#include "analysis/index/ConversionSiteFacts.h"

#include "analysis/index/SyntaxOperands.h"
#include "analysis/types/TypeSanitization.h"
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
 * @brief Binary-searches a start-ascending, end-descending fact vector for an exact byte span.
 * @tparam Fact Fact type exposing the span through `bytes`.
 * @param[in] facts Facts ordered by start byte, then by end byte descending.
 * @param[in] bytes Exact span to locate.
 * @return Pointer to the matching fact, or nullptr.
 */
template <typename Fact>
static const Fact* FindExactSpan(const std::vector<Fact>& facts, IndexedByteRange bytes) noexcept
{
    const auto it = std::lower_bound(facts.begin(), facts.end(), bytes,
                                     [](const Fact& fact, const IndexedByteRange& target)
                                     {
                                         return fact.bytes.startByte != target.startByte
                                                    ? fact.bytes.startByte < target.startByte
                                                    : fact.bytes.endByte > target.endByte;
                                     });
    if (it != facts.end() && it->bytes.startByte == bytes.startByte && it->bytes.endByte == bytes.endByte)
    {
        return &(*it);
    }
    return nullptr;
}

/**
 * @brief Orders facts by start byte ascending, then by end byte descending.
 * @tparam Fact Fact type exposing the span through `bytes`.
 * @param[in,out] facts Vector sorted in place.
 */
template <typename Fact> static void SortBySpan(std::vector<Fact>& facts)
{
    std::ranges::sort(facts,
                      [](const Fact& a, const Fact& b)
                      {
                          return a.bytes.startByte != b.bytes.startByte ? a.bytes.startByte < b.bytes.startByte
                                                                        : a.bytes.endByte > b.bytes.endByte;
                      });
}

const ConditionSiteFact* ConversionSiteFacts::FindCondition(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(conditions, bytes);
}

const TernarySiteFact* ConversionSiteFacts::FindTernary(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(ternaries, bytes);
}

const ExpressionStatementSiteFact* ConversionSiteFacts::FindExpressionStatement(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(expressionStatements, bytes);
}

const ForeachSiteFact* ConversionSiteFacts::FindForeach(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(foreachLoops, bytes);
}

const AutoDeclarationSiteFact* ConversionSiteFacts::FindAutoDeclaration(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(autoDeclarations, bytes);
}

/**
 * @brief Converts a node's start and end points to a diagnostic range.
 * @param[in] node Syntax node; may be null.
 * @return Line and column span, all zero for a null node.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return {start.row, start.column, end.row, end.column};
}

/**
 * @brief Captures the spans and spelling of one expression child.
 * @param[in] node Expression node; may be null.
 * @param[in] source Document text the node was parsed from.
 * @return The fact, or a default-constructed one when the node is null.
 */
static ConversionExpressionFact BuildExpressionFact(TSNode node, std::string_view source) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    return ConversionExpressionFact{.bytes = {ts_node_start_byte(node), ts_node_end_byte(node)},
                                    .range = RangeOf(node),
                                    .text = parser::NodeText(node, source)};
}

/**
 * @brief Reads the condition expression of an if, loop or ternary node.
 * @param[in] node Statement or ternary node.
 * @param[in] index Extracted syntax of the document.
 * @return The condition node; the second named child for do-while, else the first, when no field is present.
 */
static TSNode ReadConditionNode(TSNode node, const NodeIndex& index)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, &index);
    TSNode condition = syntax.Field(gs.fieldCondition);
    if (!ts_node_is_null(condition) && ts_node_symbol(condition) == gs.symExpressionStatement)
    {
        condition = SyntaxOperands(condition, &index).FirstNamed();
    }
    if (ts_node_is_null(condition))
    {
        condition = syntax.NamedAt(ts_node_symbol(node) == gs.symDoWhileStatement ? 1 : 0);
    }
    return condition;
}

/**
 * @brief Appends the leaf operands of a condition, looking through `&&`, `||`, `^^` and `!` chains.
 * @param[in] root Condition expression.
 * @param[in] index Extracted syntax of the document.
 * @param[in] source Document text the nodes were parsed from.
 * @param[in,out] operands Receives the leaves in left-to-right source order.
 */
static void AppendBooleanOperands(TSNode root, const NodeIndex& index, std::string_view source,
                                  std::vector<ConversionExpressionFact>& operands)
{
    const auto& gs = parser::GetGrammarSymbols();
    std::vector<TSNode> worklist{root};
    while (!worklist.empty())
    {
        const TSNode expr = worklist.back();
        worklist.pop_back();
        if (ts_node_is_null(expr))
        {
            continue;
        }

        const TSSymbol sym = ts_node_symbol(expr);
        const SyntaxOperands exprOperands(expr, &index);
        const TSNode op = exprOperands.Field(gs.fieldOperator);
        const TSSymbol opSym = ts_node_is_null(op) ? 0 : ts_node_symbol(op);
        if (sym == gs.symBinaryExpression && (opSym == gs.tokAmpAmp || opSym == gs.tokAnd || opSym == gs.tokPipePipe ||
                                              opSym == gs.tokOr || opSym == gs.tokCaretCaret || opSym == gs.tokXor))
        {
            worklist.push_back(exprOperands.Field(gs.fieldRight));
            worklist.push_back(exprOperands.Field(gs.fieldLeft));
            continue;
        }
        if (sym == gs.symUnaryExpression && (opSym == gs.tokBang || opSym == gs.tokNot))
        {
            worklist.push_back(exprOperands.Field(gs.fieldOperand));
            continue;
        }
        operands.push_back(BuildExpressionFact(expr, source));
    }
}

/**
 * @brief Captures the condition sites: if, while, do-while, for and ternary nodes with a condition.
 * @param[in] index Extracted syntax of the document.
 * @param[in] source Document text the nodes were parsed from.
 * @param[in,out] facts Receives the condition sites and their boolean operands.
 */
static void BuildConditionFacts(const NodeIndex& index, std::string_view source, ConversionSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSSymbol symbol :
         {gs.symIfStatement, gs.symWhileStatement, gs.symDoWhileStatement, gs.symForStatement, gs.symTernaryExpression})
    {
        for (const TSNode node : index.Nodes(symbol))
        {
            const TSNode condition = ReadConditionNode(node, index);
            if (ts_node_is_null(condition))
            {
                continue;
            }

            const auto* syntax = index.FindOperandSyntax(node);
            const auto operandBegin = static_cast<uint32_t>(facts.booleanOperands.size());
            AppendBooleanOperands(condition, index, source, facts.booleanOperands);
            facts.conditions.push_back(ConditionSiteFact{
                .bytes = {ts_node_start_byte(node), ts_node_end_byte(node)},
                .statementRange = RangeOf(node),
                .condition = BuildExpressionFact(condition, source),
                .unwrapped = BuildExpressionFact(UnwrapIndexedParentheses(condition, &index), source),
                .operandBegin = operandBegin,
                .operandCount = static_cast<uint32_t>(facts.booleanOperands.size()) - operandBegin,
                .isTernary = symbol == gs.symTernaryExpression,
                .isRootIf = symbol == gs.symIfStatement && (!syntax || syntax->parentSymbol != gs.symIfStatement)});
        }
    }
    SortBySpan(facts.conditions);
}

/**
 * @brief Captures ternary expressions with both branches and expression statements with an expression.
 * @param[in] index Extracted syntax of the document.
 * @param[in] source Document text the nodes were parsed from.
 * @param[in,out] facts Receives the ternary and expression-statement sites.
 */
static void BuildBranchFacts(const NodeIndex& index, std::string_view source, ConversionSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode node : index.Nodes(gs.symTernaryExpression))
    {
        const SyntaxOperands syntax(node, &index);
        const TSNode consequence = syntax.Field(gs.fieldConsequence);
        const TSNode alternative = syntax.Field(gs.fieldAlternative);
        if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
        {
            continue;
        }
        facts.ternaries.push_back(TernarySiteFact{.bytes = {ts_node_start_byte(node), ts_node_end_byte(node)},
                                                  .range = RangeOf(node),
                                                  .consequence = BuildExpressionFact(consequence, source),
                                                  .alternative = BuildExpressionFact(alternative, source)});
    }
    SortBySpan(facts.ternaries);

    for (const TSNode node : index.Nodes(gs.symExpressionStatement))
    {
        const TSNode expression = SyntaxOperands(node, &index).FirstNamed();
        if (ts_node_is_null(expression))
        {
            continue;
        }
        facts.expressionStatements.push_back(
            ExpressionStatementSiteFact{.bytes = {ts_node_start_byte(node), ts_node_end_byte(node)},
                                        .statementRange = RangeOf(node),
                                        .expression = BuildExpressionFact(expression, source)});
    }
    SortBySpan(facts.expressionStatements);
}

/**
 * @brief Captures foreach statements that have a collection, with their loop variables.
 * @param[in] index Extracted syntax of the document.
 * @param[in] source Document text the nodes were parsed from.
 * @param[in,out] facts Receives the foreach sites and variables.
 */
static void BuildForeachFacts(const NodeIndex& index, std::string_view source, ConversionSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode node : index.Nodes(gs.symForeachStatement))
    {
        const SyntaxOperands syntax(node, &index);
        const TSNode collection = syntax.Field(gs.fieldCollection);
        if (ts_node_is_null(collection))
        {
            continue;
        }

        const auto variableBegin = static_cast<uint32_t>(facts.foreachVariables.size());
        uint32_t ordinal = 0;
        syntax.ForEachNamedOperands(
            [&](const SyntaxOperands& child)
            {
                if (child.Symbol() == gs.symForeachVariable)
                {
                    const TSNode name = child.Field(gs.fieldName);
                    facts.foreachVariables.push_back(ForeachVariableFact{
                        .ordinal = ordinal++, .name = parser::NodeText(name, source), .nameRange = RangeOf(name)});
                }
                return true;
            });
        facts.foreachLoops.push_back(
            ForeachSiteFact{.bytes = {ts_node_start_byte(node), ts_node_end_byte(node)},
                            .statementRange = RangeOf(node),
                            .collection = BuildExpressionFact(collection, source),
                            .variableBegin = variableBegin,
                            .variableCount = static_cast<uint32_t>(facts.foreachVariables.size()) - variableBegin});
    }
    SortBySpan(facts.foreachLoops);
}

/**
 * @brief Captures variable declarations whose declared type is `auto`, with their declarators.
 * @param[in] index Extracted syntax of the document.
 * @param[in] source Document text the nodes were parsed from.
 * @param[in,out] facts Receives the `auto` declaration sites and declarators.
 */
static void BuildAutoFacts(const NodeIndex& index, std::string_view source, ConversionSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode node : index.Nodes(gs.symVariableDeclaration))
    {
        const SyntaxOperands syntax(node, &index);
        TSNode typeNode = syntax.Field(gs.fieldVarType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = syntax.Field(gs.fieldType);
        }
        if (CleanBaseTypeView(parser::NodeText(typeNode, source)) != "auto")
        {
            continue;
        }

        const auto declaratorBegin = static_cast<uint32_t>(facts.autoDeclarators.size());
        syntax.ForEachNamedOperands(
            [&](const SyntaxOperands& child)
            {
                if (child.Symbol() == gs.symVariableDeclarator)
                {
                    const TSNode name = child.Field(gs.fieldName);
                    facts.autoDeclarators.push_back(
                        AutoDeclaratorFact{.declaratorRange = RangeOf(child.Node()),
                                           .name = parser::NodeText(name, source),
                                           .nameRange = RangeOf(name),
                                           .value = BuildExpressionFact(child.DeclaratorValue(source), source)});
                }
                return true;
            });
        facts.autoDeclarations.push_back(AutoDeclarationSiteFact{
            .bytes = {ts_node_start_byte(node), ts_node_end_byte(node)},
            .declaratorBegin = declaratorBegin,
            .declaratorCount = static_cast<uint32_t>(facts.autoDeclarators.size()) - declaratorBegin});
    }
    SortBySpan(facts.autoDeclarations);
}

ConversionSiteFacts BuildConversionSiteFacts(const NodeIndex& index, std::string_view source)
{
    ConversionSiteFacts facts;
    BuildConditionFacts(index, source, facts);
    BuildBranchFacts(index, source, facts);
    BuildForeachFacts(index, source, facts);
    BuildAutoFacts(index, source, facts);
    return facts;
}

} // namespace angel_lsp::analysis
