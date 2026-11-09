#pragma once

#include "analysis/checkers/null_safety/NullSafetyTypes.h"
#include "analysis/index/ScopeTree.h"

#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Collects handle parameters from scope tree or AST parameter lists into initial flow state.
 * @param[in] callable Extracted function or lambda with its body position and handle parameters.
 * @param[in] ctx Lexical scopes and null flow facts for the revision.
 * @param[in,out] state Flow state to populate with nullable parameters.
 */
void CollectParameters(const NullFlowCallable& callable, const NullCheckContext& ctx, FlowState& state);

} // namespace angel_lsp::analysis
