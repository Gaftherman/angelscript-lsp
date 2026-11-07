#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ReferenceCounting.h"

#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Checks whether a member expression reference targets a compatible receiver class.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] criteria Active search criteria.
 * @return True if receiver class is compatible.
 */
[[nodiscard]] bool IsCompatibleMemberAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                                            const ReferenceCollectionCriteria& criteria);
} // namespace angel_lsp::analysis
