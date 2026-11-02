#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include <ankerl/unordered_dense.h>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
class NodeIndex;
struct Scope;

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
 * @brief Context parameters for null safety handle flow analysis.
 */
struct NullSafetyCheckRequest
{
    TSNode root;
    std::string_view sourceCode;
    const Scope* scopeRoot = nullptr;
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Evaluation context passed during null-safety traversal.
 */
struct NullCheckContext
{
    std::string_view sourceCode;
    DiagnosticContext& diagCtx;
    const Scope* scopeRoot = nullptr;
};

/**
 * @brief Dataflow state at a specific control flow point.
 */
struct FlowState
{
    ankerl::unordered_dense::map<std::string, Nullability> vars;
    ankerl::unordered_dense::set<std::string> warnedVars;
    bool isTerminated = false;
};

/**
 * @brief Guard condition assertion extracted from branch conditions.
 */
struct NullAssertion
{
    std::string varName;
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

} // namespace angel_lsp::analysis
