#pragma once

#include "analysis/index/SymbolTable.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context for the request that opens a type hierarchy.
 */
struct TypeHierarchyPrepareRequest
{
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    const analysis::SymbolTable& symbolTable;
    lsp::Position position;
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
std::optional<std::vector<lsp::TypeHierarchyItem>> PrepareTypeHierarchy(const TypeHierarchyPrepareRequest& request);

/**
 * @brief The types this one declares as its bases.
 * @note Direct bases only. The client expands the tree one level at a time, and answering
 *        transitively would list every ancestor again under each of its own descendants.
 */
std::optional<std::vector<lsp::TypeHierarchyItem>> GetSupertypes(const TypeHierarchyItemRequest& request);

/**
 * @brief The types that declare this one among their bases.
 * @note Direct subtypes only, for the same reason as GetSupertypes.
 */
std::optional<std::vector<lsp::TypeHierarchyItem>> GetSubtypes(const TypeHierarchyItemRequest& request);

/**
 * @brief Parameters and traversal filters for finding subtypes in a type hierarchy.
 */
struct SubtypeQueryContext
{
    std::string target;
    std::string qualifiedTarget;
    std::string itemName;
    const analysis::SymbolTable& symbolTable;
};
} // namespace angel_lsp::features
