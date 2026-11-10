#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/checkers/IsolationChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/IsolationSiteFacts.h"
#include "analysis/index/ScopeTree.h"

namespace angel_lsp::analysis
{

/**
 * @brief Verifies that the 'shared' modifier is only attached to allowed entity declarations.
 * @param[in] facts Precalculated pass-1 isolation site facts.
 * @param[in,out] ctx Diagnostic context used to emit error diagnostics for invalid shared entities.
 */
void CheckSharedEntityEligibility(const IsolationSiteFacts& facts, DiagnosticContext& ctx);

/**
 * @brief Validates that a datatype referenced within a shared context resolves to a shared entity or primitive.
 * @param[in] fact Precalculated datatype site fact.
 * @param[in,out] ctx Diagnostic context to emit findings.
 */
void CheckSharedDatatype(const IsolationTypeSiteFact& fact, DiagnosticContext& ctx);

/**
 * @brief Validates a function call expression within a shared context to ensure the target function is shared.
 * @param[in] fact Precalculated call site fact.
 * @param[in,out] ctx Diagnostic context to emit findings.
 */
void CheckSharedCallExpression(const IsolationCallSiteFact& fact, DiagnosticContext& ctx);

/**
 * @brief Validates that an identifier read inside a shared context does not access a non-shared global variable.
 * @param[in] fact Precalculated identifier site fact.
 * @param[in] scopeRoot Document scope tree root used for lexical resolution.
 * @param[in,out] ctx Diagnostic context to emit findings.
 */
void CheckSharedIdentifier(const IsolationIdentifierSiteFact& fact, const Scope* scopeRoot, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
