#pragma once

#include "analysis/checkers/null_safety/NullSafetyTypes.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
struct Scope;

/**
 * @brief Checks expressions for potential null handle dereferences.
 * @param[in] expr Expression AST node to check.
 * @param[in,out] state Dataflow state tracking handle nullability.
 * @param[in,out] ctx Diagnostic context and source code view.
 */
void CheckNullExpression(TSNode expr, FlowState& state, NullCheckContext& ctx);

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
    TSNode node;
    decltype(FlowState::vars) savedVars;
};
} // namespace angel_lsp::analysis
