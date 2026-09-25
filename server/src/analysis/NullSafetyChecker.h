#pragma once

#include "analysis/DiagnosticContext.h"
#include "analysis/NullSafetyTypes.h"

namespace angel_lsp::analysis
{
/**
 * @brief Analyzes dataflow in functions and lambdas to detect dereferences of null or unchecked handles.
 * @param[in] request Document AST root, source text, and scope tree.
 * @param[in,out] ctx Diagnostic sink for emitting warnings.
 */
void CheckNullSafety(const NullSafetyCheckRequest& request, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
