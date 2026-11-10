#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/MemberAccessFacts.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Walks the document and reports member accesses the declaring class does not permit.
 * @param[in] request Tree, source text and scope tree of the document under analysis.
 * @param[in,out] ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void CheckMemberAccess(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief Validates member accesses against pre-extracted AST-free member access facts.
 * @param[in] facts Pre-extracted member access, identifier, and closure facts of the document.
 * @param[in] request Ast check context providing lexical scopes and mediator.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckMemberAccess(const MemberAccessFacts& facts, const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief AST checker for class and interface member access visibility.
 */
struct AccessChecker
{
    static constexpr std::string_view Name = "AccessChecker";
    static bool IsEnabled([[maybe_unused]] const SemanticAnalysisRequest& request)
    {
        return true;
    }
    static void Run(const AstCheckContext& ast, DiagnosticContext& ctx)
    {
        CheckMemberAccess(ast, ctx);
    }
};

} // namespace angel_lsp::analysis
