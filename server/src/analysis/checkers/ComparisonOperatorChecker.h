#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/ScopeTree.h"

namespace angel_lsp::analysis
{
/**
 * @brief Validates operand type compatibility for equality and relational comparisons.
 * @param[in] fact Binary expression facts with operator and both operands.
 * @param[in] scope Lexical scope enclosing the comparison.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckComparisonOperatorCompatibility(const BinaryExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx);
} // namespace angel_lsp::analysis
