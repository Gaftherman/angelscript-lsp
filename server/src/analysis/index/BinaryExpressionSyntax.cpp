#include "analysis/index/BinaryExpressionSyntax.h"

#include "analysis/index/SyntaxOperands.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Reads the operator and operand nodes of a binary expression syntax node.
 * @param[in] node Binary expression syntax node.
 * @param[in] index Extracted syntax index, or nullptr when unavailable.
 * @return Operator and operand nodes where absent parts are null nodes.
 */
BinaryExpressionOperands ReadBinaryOperands(TSNode node, const NodeIndex* index)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, index);
    return {syntax.Field(gs.fieldOperator), syntax.Field(gs.fieldLeft), syntax.Field(gs.fieldRight)};
}

/**
 * @brief Collects condition nodes along the alternative links of an if ladder.
 * @param[in] rootIf First conditional statement node.
 * @param[in] index Extracted syntax index, or nullptr when unavailable.
 * @return Condition nodes in source order.
 */
std::vector<TSNode> CollectIfLadderConditions(TSNode rootIf, const NodeIndex* index)
{
    std::vector<TSNode> conditions;
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode current = rootIf; !ts_node_is_null(current) && ts_node_symbol(current) == gs.symIfStatement;)
    {
        const SyntaxOperands syntax(current, index);
        const TSNode condition = syntax.FieldOrNamed(gs.fieldCondition, 0);
        if (!ts_node_is_null(condition))
        {
            conditions.push_back(condition);
        }
        current = syntax.Field(gs.fieldAlternative);
    }
    return conditions;
}

/**
 * @brief Follows parentheses and unary operators down to a literal.
 * @param[in] node Expression syntax node.
 * @param[in] index Extracted syntax for diagnostics, or nullptr for an interactive query.
 * @return True when the expression is a number, string, boolean or null literal, possibly wrapped.
 */
bool IsNumericLiteralExpression(TSNode node, const NodeIndex* index)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode current = node;
    while (!ts_node_is_null(current))
    {
        const TSSymbol sym = ts_node_symbol(current);
        if (sym == gs.symNumberLiteral || sym == gs.symStringLiteral || sym == gs.symBooleanLiteral ||
            sym == gs.symNullLiteral)
        {
            return true;
        }
        if (sym != gs.symParenthesizedExpression && sym != gs.symUnaryExpression)
        {
            return false;
        }
        current = SyntaxOperands(current, index).FirstNamed();
        if (ts_node_is_null(current))
        {
            return false;
        }
    }
    return false;
}

} // namespace angel_lsp::analysis
