#pragma once

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Validates operand type compatibility for non-comparison binary operators
 *        (bitwise, arithmetic, modulo).
 * @param[in] operands Complete operator, left and right handles for the expression.
 * @param[in] scope Lexical scope enclosing the binary expression.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckBinaryOperatorCompatibility(const BinaryExpressionOperands& operands, const Scope* scope,
                                      DiagnosticContext& ctx);
} // namespace angel_lsp::analysis
