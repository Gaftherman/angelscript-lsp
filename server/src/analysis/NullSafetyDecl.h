#pragma once

#include "analysis/NullSafetyExpr.h"
#include "analysis/NullSafetyTypes.h"
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks a variable declaration statement for handle variables and initialization.
 * @param[in] stmt Variable declaration AST node.
 * @param[in,out] state Flow state updated with declared variables and their nullability.
 * @param[in,out] ctx Null check context for source code and diagnostic emission.
 */
void CheckVarDeclaration(TSNode stmt, FlowState& state, NullCheckContext& ctx);

/**
 * @brief Checks an assignment expression for handle reassignment.
 * @param[in] expr Assignment expression AST node.
 * @param[in,out] state Flow state updated with reassigned nullability.
 * @param[in,out] ctx Null check context for source code and diagnostic emission.
 */
void CheckAssignmentStmt(TSNode expr, FlowState& state, NullCheckContext& ctx);

} // namespace angel_lsp::analysis
