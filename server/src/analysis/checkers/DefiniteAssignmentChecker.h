#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/DefiniteAssignmentFacts.h"

#include <ankerl/unordered_dense.h>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Analyzes dataflow in function bodies to detect reads of uninitialized local variables.
 * @param[in] request Document AST root, source text, and scope tree.
 * @param[in,out] ctx Diagnostic sink for emitting warnings/errors.
 */
void CheckDefiniteAssignment(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief Validates definite assignment directly against precalculated pass-1 definite assignment facts.
 * @param[in] facts Precalculated pass-1 definite assignment facts.
 * @param[in] request Document AST root, source text, and scope tree.
 * @param[in,out] ctx Diagnostic sink for emitting warnings/errors.
 */
void CheckDefiniteAssignment(const DefiniteAssignmentFacts& facts, const AstCheckContext& request,
                             DiagnosticContext& ctx);

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
    ankerl::unordered_dense::set<std::string_view> assignedVars;
    bool hasReturned = false;
    bool isTerminated = false;
};

/**
 * @brief Holds a pending statement and the branch states borrowed by its descendants on the worklist.
 */
struct AssignmentWorkFrame
{
    uint32_t recordIndex = UINT32_MAX;
    DefiniteAssignmentState& state;
    DefiniteAssignmentState branch;
    DefiniteAssignmentState alternate;
    uint32_t nextChild = 0;
    uint8_t phase = 0;
};

/**
 * @brief The chosen signature and the viable candidates used to classify out parameters.
 */
struct CallCandidateInfo
{
    const std::vector<Symbol>& candidates;
    const FunctionSignature* sig = nullptr;
};

} // namespace angel_lsp::analysis
