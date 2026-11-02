#pragma once

#include "analysis/checkers/null_safety/NullSafetyTypes.h"
#include "analysis/diagnostics/DiagnosticContext.h"

namespace angel_lsp::analysis
{
/**
 * @brief Analyzes dataflow in functions and lambdas to detect dereferences of null or unchecked handles.
 * @param[in] request Document AST root, source text, and scope tree.
 * @param[in,out] ctx Diagnostic sink for emitting warnings.
 */
void CheckNullSafety(const NullSafetyCheckRequest& request, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
