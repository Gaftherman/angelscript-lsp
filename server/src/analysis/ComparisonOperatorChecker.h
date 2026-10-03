#pragma once

#include "analysis/BinaryOperatorHelpers.h"
#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Validates operand type compatibility for binary comparison operators.
 * @param[in] node Syntax tree node of a binary_expression.
 * @param[in] scope Lexical scope enclosing the binary expression.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckComparisonOperatorCompatibility(TSNode node, const Scope* scope, DiagnosticContext& ctx);
} // namespace angel_lsp::analysis
