#pragma once

#include "analysis/index/SymbolTable.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context for the request that opens a type hierarchy.
 */
struct TypeHierarchyPrepareRequest
{
    std::string_view uri{};
    std::string_view sourceCode{};
    const analysis::SymbolTable& symbolTable;
    lsp::Position position{};
};

/**
 * @brief Context for a follow-up supertypes or subtypes request.
 *        The client hands back the item it was given, so the symbol table is all that is needed to
 */
struct TypeHierarchyItemRequest
{
    const analysis::SymbolTable& symbolTable;
    const lsp::TypeHierarchyItem& item;
};

/**
 * @brief Opens a type hierarchy on the type the cursor is looking at.
 *        The identifier under the cursor when it names a class or an interface; failing that, the type
 * @return The item, or nullopt when the cursor is looking at no type at all.
 */
[[nodiscard]] std::optional<std::vector<lsp::TypeHierarchyItem>>
PrepareTypeHierarchy(const TypeHierarchyPrepareRequest& request);

/**
 * @brief The types this one declares as its bases.
 * @note Direct bases only. The client expands the tree one level at a time, and answering
 *        transitively would list every ancestor again under each of its own descendants.
 */
[[nodiscard]] std::optional<std::vector<lsp::TypeHierarchyItem>> GetSupertypes(const TypeHierarchyItemRequest& request);

/**
 * @brief The types that declare this one among their bases.
 * @note Direct subtypes only, for the same reason as GetSupertypes.
 */
[[nodiscard]] std::optional<std::vector<lsp::TypeHierarchyItem>> GetSubtypes(const TypeHierarchyItemRequest& request);

/**
 * @brief Parameters and traversal filters for finding subtypes in a type hierarchy.
 */
struct SubtypeQueryContext
{
    std::string_view target;          ///< Bare name of the queried type; borrows from the request item.
    std::string_view qualifiedTarget; ///< Qualified name from the item's data payload, or empty.
    std::string_view itemName;        ///< Display name of the request item.
    const analysis::SymbolTable& symbolTable;
};
} // namespace angel_lsp::features
