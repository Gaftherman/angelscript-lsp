#pragma once

#include "analysis/DiagnosticContext.h"
#include "analysis/NullSafetyTypes.h"
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
struct Scope;

struct NullCheckContext
{
    std::string_view sourceCode;
    DiagnosticContext& diagCtx;
    const Scope* scopeRoot = nullptr;
};

/**
 * @brief Checks expressions for potential null handle dereferences.
 * @param[in] expr Expression AST node to check.
 * @param[in,out] state Dataflow state tracking handle nullability.
 * @param[in,out] ctx Diagnostic context and source code view.
 */
void CheckNullExpression(TSNode expr, FlowState& state, NullCheckContext& ctx);

} // namespace angel_lsp::analysis
