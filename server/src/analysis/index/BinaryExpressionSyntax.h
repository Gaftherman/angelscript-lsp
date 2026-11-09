#pragma once

#include "analysis/index/NodeIndex.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Operator token and both operands of a binary expression node.
 */
struct BinaryExpressionOperands
{
    TSNode op;
    TSNode left;
    TSNode right;

    /**
     * @brief Reports whether the operator and both operands were captured.
     * @return True when none of the three nodes is null.
     */
    [[nodiscard]] bool Complete() const noexcept
    {
        return !ts_node_is_null(op) && !ts_node_is_null(left) && !ts_node_is_null(right);
    }
};

/**
 * @brief Reads the operator and operands of a binary expression node.
 * @param[in] node Binary expression syntax node.
 * @param[in] index Extracted syntax for diagnostics, or nullptr for an interactive query.
 * @return Operator and operand nodes; absent parts are null nodes.
 */
[[nodiscard]] BinaryExpressionOperands ReadBinaryOperands(TSNode node, const NodeIndex* index);

/**
 * @brief Collects condition nodes along the alternative links of an if-else ladder.
 * @param[in] rootIf First conditional statement.
 * @param[in] index Extracted syntax for diagnostics, or nullptr for an interactive query.
 * @return Nonempty conditions in source order, excluding the final else body.
 */
[[nodiscard]] std::vector<TSNode> CollectIfLadderConditions(TSNode rootIf, const NodeIndex* index);

/**
 * @brief Follows parentheses and unary operators down to a literal.
 * @param[in] node Expression syntax node.
 * @param[in] index Extracted syntax for diagnostics, or nullptr for an interactive query.
 * @return True when the expression is a number, string, boolean or null literal, possibly wrapped.
 */
[[nodiscard]] bool IsNumericLiteralExpression(TSNode node, const NodeIndex* index);

} // namespace angel_lsp::analysis
