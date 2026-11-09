#pragma once

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Syntactic position a lambda expression occupies, which decides how its funcdef target is found.
 */
enum class LambdaTargetKind : uint8_t
{
    None,           ///< The lambda sits in a position that names no funcdef.
    DeclaratorType, ///< Initializer of a variable declarator; `text` is the declared type.
    AssignmentLeft, ///< Right side of an assignment; `text` is the assigned expression.
    CallArgument    ///< Argument of a call; `text` is the callee and `position` the argument index.
};

/**
 * @brief AST-free description of where a lambda expression appears, extracted once from the syntax tree.
 * @note `text` views the analysed document text, which must outlive the fact.
 */
struct LambdaTargetFact
{
    LambdaTargetKind kind = LambdaTargetKind::None;
    std::string_view text;     ///< Raw source spelling selected by `kind`.
    uint32_t position = 0;     ///< Named-child index inside the argument list; CallArgument only.
    bool soleArgument = false; ///< The argument list holds exactly one named child; CallArgument only.
};

/**
 * @brief Reads the declaration, assignment or call context of a lambda expression.
 * @param[in] lambdaNode Lambda expression node; any other node yields an empty fact.
 * @param[in] source Document text that the returned views point into.
 * @return Fact with kind None when the lambda has no usable parent context.
 * @note Cost is O(k) for the k named children of the enclosing argument list, with bounded parent hops.
 */
[[nodiscard]] LambdaTargetFact ReadLambdaTarget(TSNode lambdaNode, std::string_view source);

} // namespace angel_lsp::analysis
