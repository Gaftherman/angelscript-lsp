#pragma once

#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Validates operand type compatibility for non-comparison binary operators
 *        (bitwise, arithmetic, modulo).
 * @param[in] node Syntax tree node of a binary_expression.
 * @param[in] scope Lexical scope enclosing the binary expression.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckBinaryOperatorCompatibility(TSNode node, const Scope* scope, DiagnosticContext& ctx);
} // namespace angel_lsp::analysis
