#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/IsolationSiteFacts.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Validates strict isolation boundaries: shared entities cannot access non-shared entities.
 * @param[in] request Immutable context with AST, source code, and scopes.
 * @param[in,out] ctx Diagnostic context to emit findings.
 */
void CheckSharedIsolation(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief Validates strict isolation boundaries against precalculated pass-1 isolation facts.
 * @param[in] facts Precalculated pass-1 isolation site facts.
 * @param[in] request Immutable context with source code and scope tree.
 * @param[in,out] ctx Diagnostic context to emit findings.
 */
void CheckSharedIsolation(const IsolationSiteFacts& facts, const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief AST checker for strict shared isolation boundaries.
 */
struct IsolationChecker
{
    static constexpr std::string_view Name = "IsolationChecker";
    static bool IsEnabled([[maybe_unused]] const SemanticAnalysisRequest& request)
    {
        return true;
    }
    static void Run(const AstCheckContext& ast, DiagnosticContext& ctx)
    {
        CheckSharedIsolation(ast, ctx);
    }
};

} // namespace angel_lsp::analysis
