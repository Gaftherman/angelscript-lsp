#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/checkers/LValueChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/LValueSiteFacts.h"
#include "analysis/index/ScopeTree.h"

namespace angel_lsp::analysis
{

/**
 * @brief Validates whether a call expression target is a modifiable reference l-value.
 * @param[in] callSite Pre-extracted call site fact.
 * @param[in] request Contextual request bundle containing source text and parameters.
 * @param[in] scope Lexical scope active at the call site.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckCallLValue(const LValueCallSiteFact& callSite, const AstCheckContext& request, const Scope* scope,
                     DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
