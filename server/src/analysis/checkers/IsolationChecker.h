#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include <string>
#include <tree_sitter/api.h>

#include "analysis/checkers/AstCheckContext.h"

namespace angel_lsp::analysis
{
class NodeIndex;

using IsolationCheckRequest = AstCheckContext;

/**
 * @brief Validates strict isolation boundaries: shared entities cannot access non-shared entities.
 * @param request Immutable context with AST, source code, and scopes.
 * @param ctx Diagnostic context to emit findings.
 */
void CheckSharedIsolation(const IsolationCheckRequest& request, DiagnosticContext& ctx);

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

/**
 * @brief Expression node and evaluation barrier state during variable isolation analysis.
 */
struct IsoItem
{
    TSNode node;
    std::string className;
    bool isClassShared = false;
    bool inSharedContext = false;
};
} // namespace angel_lsp::analysis
