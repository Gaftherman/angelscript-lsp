#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/SemanticAnalysisRequest.h"

namespace angel_lsp::analysis
{
struct DiagnosticContext;
}

namespace angel_lsp::analysis::rules
{

/**
 * @brief Checks for unused local variables, member variables, global variables, functions, and classes.
 * @param[in] request Semantic analysis request containing AST, symbol table, and engine rules.
 * @param[in,out] ctx Diagnostic context accumulating emitted warnings.
 */
void CheckUnusedSymbols(const SemanticAnalysisRequest& request, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis::rules
