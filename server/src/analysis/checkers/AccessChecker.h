#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"

namespace angel_lsp::analysis
{
class NodeIndex;

using AccessCheckRequest = AstCheckContext;

/**
 * @brief Walks the document and reports member accesses the declaring class does not permit.
 * @param request Tree, source text and scope tree of the document under analysis.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void CheckMemberAccess(const AccessCheckRequest& request, DiagnosticContext& ctx);

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
