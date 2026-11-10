#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/LValueSiteFacts.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Verifies that left-hand side targets of assignment expressions are modifiable l-values.
 * @param request Syntax tree, source code, and lexical scope.
 * @param ctx Diagnostic sink and context.
 */
void CheckLValues(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief AST checker for left-hand side modifiable l-value expressions.
 */
struct LValueChecker
{
    static constexpr std::string_view Name = "LValueChecker";
    static bool IsEnabled([[maybe_unused]] const SemanticAnalysisRequest& request)
    {
        return true;
    }
    static void Run(const AstCheckContext& ast, DiagnosticContext& ctx)
    {
        CheckLValues(ast, ctx);
    }
};

/**
 * @brief Bundles callee candidate signatures, argument expressions, and typing state during call evaluation.
 */
struct CallCandidateContext
{
    const AstCheckContext& request;
    const Scope* scope = nullptr;
    DiagnosticContext& ctx;
};

/**
 * @brief Evaluation engine for validating assignable L-value expression nodes.
 */
struct LValueNodeValidator
{
    /**
     * @brief Determines whether an expression fact denotes an assignable modifiable L-value.
     * @param[in] candidate Pre-extracted l-value candidate fact.
     * @param[in] facts Document-wide l-value site facts for branch candidate resolution.
     * @param[in] scope Lexical scope.
     * @param[in] ctx Diagnostic context.
     * @return True if candidate is an assignable L-value; false otherwise.
     */
    static bool IsAssignableLValueNode(const LValueCandidateFact& candidate, const LValueSiteFacts& facts,
                                       const Scope* scope, const DiagnosticContext& ctx);
};
} // namespace angel_lsp::analysis
