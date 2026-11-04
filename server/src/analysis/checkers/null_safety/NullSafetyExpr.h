#pragma once

#include "analysis/checkers/null_safety/NullSafetyCondition.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks expressions for potential null handle dereferences.
 * @param[in] expr Expression AST node to check.
 * @param[in,out] state Dataflow state tracking handle nullability.
 * @param[in,out] ctx Diagnostic context and source code view.
 * @param[out] assertions Facts for the whole expression when requested.
 */
void CheckNullExpression(TSNode expr, FlowState& state, NullCheckContext& ctx,
                         NullConditionAssertions* assertions = nullptr);

/**
 * @brief Action directive for evaluating intermediate expressions during null-safety traversal.
 */
enum class ExprActionKind : uint8_t
{
    Eval,
    AfterLeftAnd,
    AfterLeftOr,
    RestoreVars
};

/**
 * @brief Stack frame holding intermediate subexpression nodes and typing qualifiers during null-safety traversal.
 */
struct ExprStackFrame
{
    ExprActionKind kind;
    TSNode node{};
    TSNode right{};
    decltype(FlowState::vars) savedVars{};
};
} // namespace angel_lsp::analysis
