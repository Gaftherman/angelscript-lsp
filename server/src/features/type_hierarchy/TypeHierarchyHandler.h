#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolTable.h"
#include "utils/PositionEncoding.h"

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
    std::string_view uri;                     ///< Canonical URI of the cursor document.
    std::string_view sourceCode;              ///< Client-visible text for position conversion.
    std::string_view parsedSource;            ///< Parser input with the same line and byte offsets.
    const analysis::SymbolTable& symbolTable; ///< Indexed workspace declarations.
    const analysis::NodeIndex& nodeIndex;     ///< Syntax facts for the parsed source.
    lsp::Position position;                   ///< Cursor in the client's negotiated encoding.
    utils::PositionEncoding positionEncoding; ///< Encoding used by the cursor column.
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
 * @brief Opens a type hierarchy for the referenced type or the innermost enclosing declaration.
 * @param[in] request Cursor, source revision, and declaration lookup context.
 * @return Matching items, or nullopt when the cursor resolves to no type.
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
