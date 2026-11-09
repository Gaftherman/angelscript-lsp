#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NullFlowFacts.h"
#include "analysis/index/ScopeTree.h"

#include <ankerl/unordered_dense.h>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Three-state lattice for variable nullability tracking.
 */
enum class Nullability
{
    NonNull,       ///< Variable is confirmed non-null (guarded or instantiated).
    Nullable,      ///< Variable may be null (handle parameter, cast, or unverified call).
    DefinitelyNull ///< Variable is definitely null (explicit null assignment or null branch).
};

/**
 * @brief Evaluation context passed during null-safety traversal.
 */
struct NullCheckContext
{
    std::string_view sourceCode;
    DiagnosticContext& diagCtx;
    const Scope* scopeRoot;
    const NullFlowFacts& facts;
};

/**
 * @brief Dataflow state at a specific control flow point.
 */
struct FlowState
{
    ankerl::unordered_dense::map<std::string_view, Nullability> vars;
    ankerl::unordered_dense::set<std::string_view> warnedVars;
    bool isTerminated = false;
};

/**
 * @brief Guard condition assertion extracted from branch conditions.
 */
struct NullAssertion
{
    std::string_view varName;
    Nullability state = Nullability::Nullable;
};

/**
 * @brief Applies null assertion outcomes to a dataflow state.
 * @param[in,out] state Dataflow state tracking handle nullability.
 * @param[in] assertions List of extracted null assertions to apply.
 */
inline void ApplyAssertions(FlowState& state, const std::vector<NullAssertion>& assertions)
{
    for (const auto& a : assertions)
    {
        state.vars[a.varName] = a.state;
    }
}

/// Worklist actions for null checking.
enum class NullStatementAction : uint8_t
{
    Statement,
    Children,
    Else,
    Join
};

/// A pending step in the null checking worklist.
struct NullStatementWork
{
    NullStatementAction action;
    NullFlowNodeId node = NoNullFlowNode;
    NullFlowNodeId child = NoNullFlowNode;
};

/// Separate states retained while evaluating the arms of an if statement.
struct NullBranchState
{
    FlowState thenState;
    FlowState elseState;
};

} // namespace angel_lsp::analysis
