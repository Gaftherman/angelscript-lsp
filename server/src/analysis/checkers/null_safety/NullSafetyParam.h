#pragma once

#include "analysis/checkers/null_safety/NullSafetyTypes.h"
#include "analysis/index/ScopeTree.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Locates the function or lambda body statement block.
 * @param[in] funcNode Function or lambda AST node.
 * @param[in] index Captured syntax facts for the same revision.
 * @return Statement block node, or null node if none found.
 */
TSNode FindFunctionBody(TSNode funcNode, const NodeIndex& index);

/**
 * @brief Collects handle parameters from scope tree or AST parameter lists into initial flow state.
 * @param[in] funcNode Function or lambda AST node.
 * @param[in] body Function body located in the syntax index.
 * @param[in] ctx Lexical scopes and indexed operands for the revision.
 * @param[in,out] state Flow state to populate with nullable parameters.
 */
void CollectParameters(TSNode funcNode, TSNode body, const NullCheckContext& ctx, FlowState& state);

} // namespace angel_lsp::analysis
