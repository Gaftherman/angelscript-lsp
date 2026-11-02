#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include <string_view>
#include <tree_sitter/api.h>

#include "analysis/checkers/AstCheckContext.h"

namespace angel_lsp::analysis
{
class NodeIndex;

using DefiniteAssignmentCheckRequest = AstCheckContext;

/**
 * @brief Analyzes dataflow in function bodies to detect reads of uninitialized local variables.
 * @param request Document AST root, source text, and scope tree.
 * @param ctx Diagnostic sink for emitting warnings/errors.
 */
void CheckDefiniteAssignment(const DefiniteAssignmentCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief AST checker for definite assignment and uninitialized local reads.
 */
struct DefiniteAssignmentChecker
{
    static constexpr std::string_view Name = "DefiniteAssignmentChecker";
    static bool IsEnabled([[maybe_unused]] const SemanticAnalysisRequest& request)
    {
        return true;
    }
    static void Run(const AstCheckContext& ast, DiagnosticContext& ctx)
    {
        CheckDefiniteAssignment(ast, ctx);
    }
};

/**
 * @brief Tracks definite assignment status and initialization states for variables across control flow branches.
 */
struct DefiniteAssignmentState
{
    ankerl::unordered_dense::set<std::string> assignedVars;
    bool hasReturned = false;
    bool isTerminated = false;
};

/**
 * @brief Bundles candidate callee symbol and argument expression nodes during definite assignment analysis.
 */
struct CallCandidateInfo
{
    const std::vector<Symbol>& candidates;
    const FunctionSignature* sig = nullptr;
};
} // namespace angel_lsp::analysis
