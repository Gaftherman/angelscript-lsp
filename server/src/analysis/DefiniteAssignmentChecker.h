#pragma once

#include "analysis/DiagnosticContext.h"
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
class NodeIndex;

/**
 * @brief Context parameters for definite assignment dataflow analysis.
 */
struct DefiniteAssignmentCheckRequest
{
    TSNode root;
    std::string_view sourceCode;
    const Scope* scopeRoot = nullptr;
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Analyzes dataflow in function bodies to detect reads of uninitialized local variables.
 * @param request Document AST root, source text, and scope tree.
 * @param ctx Diagnostic sink for emitting warnings/errors.
 */
void CheckDefiniteAssignment(const DefiniteAssignmentCheckRequest& request, DiagnosticContext& ctx);

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
