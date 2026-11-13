#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Parameter of a lambda expression as written in its parameter list.
 */
struct LambdaParameter
{
    std::string typeName;
    ParameterModifier modifier = ParameterModifier::None;
    bool hasWrittenType : 1 = false;
    bool isConst : 1 = false;
    bool isHandle : 1 = false;
    bool isReference : 1 = false;
};

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
 * @brief Position of one lambda parameter name and whether the source wrote its type.
 */
struct LambdaParameterSite
{
    uint32_t startLine = 0;      ///< Zero-based line of the parameter name.
    uint32_t startCharacter = 0; ///< Zero-based column of the parameter name.
    bool hasWrittenType = false; ///< The parameter spells its own type, so nothing is deduced for it.
};

/**
 * @brief AST-free description of a lambda expression and its syntactic target context.
 */
struct LambdaSiteFact
{
    IndexedByteRange bytes;                      ///< Byte span of the lambda expression.
    LambdaTargetFact target;                     ///< Syntactic target context describing where the lambda appears.
    std::vector<LambdaParameterSite> parameters; ///< Parameters in declaration order.
};

/**
 * @brief Reads the declaration, assignment or call context of a lambda expression.
 * @param[in] lambdaNode Lambda expression node; any other node yields an empty fact.
 * @param[in] source Document text that the returned views point into.
 * @return Fact with kind None when the lambda has no usable parent context.
 * @note Cost is O(k) for the k named children of the enclosing argument list, with bounded parent hops.
 */
[[nodiscard]] LambdaTargetFact ReadLambdaTarget(TSNode lambdaNode, std::string_view source);

/**
 * @brief Extracts every lambda expression and its syntactic target context from the document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return Facts ordered by occurrence in the document.
 */
[[nodiscard]] std::vector<LambdaSiteFact> BuildLambdaSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
