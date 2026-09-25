#pragma once

#include "analysis/NullSafetyTypes.h"
#include "analysis/ScopeTree.h"
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Locates the function or lambda body statement block.
 * @param[in] funcNode Function or lambda AST node.
 * @return Statement block node, or null node if none found.
 */
TSNode FindFunctionBody(TSNode funcNode);

/**
 * @brief Collects handle parameters from scope tree or AST parameter lists into initial flow state.
 * @param[in] funcNode Function or lambda AST node.
 * @param[in] scopeRoot Precomputed local scope tree root.
 * @param[in] sourceCode Source text of the document.
 * @param[in,out] state Flow state to populate with nullable parameters.
 */
void CollectParameters(TSNode funcNode, const Scope* scopeRoot, std::string_view sourceCode, FlowState& state);

} // namespace angel_lsp::analysis
