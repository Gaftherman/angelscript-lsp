#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ReferenceCounting.h"

#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Determines if a reference matches a known declaration or definition site.
 * @param[in] fileUri URI of document owning scope.
 * @param[in] ref Reference to check.
 * @param[in] scope Lexical scope of the reference.
 * @param[in] criteria Active search criteria.
 * @return True if reference represents a declaration or definition site.
 */
[[nodiscard]] bool IsDeclarationOrDefinition(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                                             const ReferenceCollectionCriteria& criteria);

/**
 * @brief Checks whether a reference has valid accessibility and scope visibility.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return True if access is valid.
 */
[[nodiscard]] bool IsValidAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                                 const ReferenceCollectionCriteria& criteria);
} // namespace angel_lsp::analysis
