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
void CheckNullSafety(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief AST checker for handle flow and null safety dereference validation.
 */
struct NullSafetyChecker
{
    static constexpr std::string_view Name = "NullSafetyChecker";
    static bool IsEnabled(const SemanticAnalysisRequest& request)
    {
        return request.diagnostics && request.diagnostics->reportPossibleNullDereference;
    }
    static void Run(const AstCheckContext& ast, DiagnosticContext& ctx)
    {
        CheckNullSafety(ast, ctx);
    }
};

} // namespace angel_lsp::analysis
