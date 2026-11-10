#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Shape of the leaf an expression evaluates to once parentheses and unary operators are removed.
 */
enum class ValueExpressionKind : uint8_t
{
    None,       ///< Unsupported leaf or missing operand; the type is unknown.
    Poison,     ///< The expression contains a syntax error.
    Number,     ///< Number literal.
    String,     ///< String literal.
    Boolean,    ///< Boolean literal.
    Null,       ///< Null literal.
    Identifier, ///< Identifier or scoped identifier.
    Call,       ///< Call or construct call whose callee was found.
    Cast,       ///< Explicit or functional cast.
    Member      ///< Member expression.
};

/**
 * @brief Leaf and unary context needed to compute the value type of an expression.
 */
struct ValueExpressionFact
{
    ValueExpressionKind kind = ValueExpressionKind::None;
    IndexedByteRange bytes;  ///< Byte span of the leaf; positions scope and container lookups.
    uint16_t nodeSymbol = 0; ///< Grammar symbol of the leaf; selects the container lookup context.
    std::string_view text;   ///< Number and Identifier: spelling. Call: callee spelling. Cast: written type.
    bool logicalNot = false; ///< A `!` or `not` operator sits above the leaf; it makes the value `bool`.
    bool negated = false;    ///< A unary `-` sits above the leaf; it swaps an unsigned type for its signed twin.
};

/**
 * @brief Captures the leaf kind, spelling and unary operators of a value expression.
 * @param[in] node Expression node; a null node yields a fact of kind None.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; the fact's views point into it.
 * @return Value expression fact. Parentheses are unwrapped, unary operators are folded into flags,
 *         and a syntax error at any unwrapped level yields kind Poison.
 * @note Walks the unary chain iteratively; cost is the chain length.
 */
[[nodiscard]] ValueExpressionFact ReadValueExpression(TSNode node, const NodeIndex* index, std::string_view source);

} // namespace angel_lsp::analysis
