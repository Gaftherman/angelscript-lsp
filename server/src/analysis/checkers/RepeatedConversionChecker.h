#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/ScopeTree.h"

namespace angel_lsp::analysis
{
/**
 * @brief Checks for repeated implicit conversions in conditional statement ladders.
 * @param[in] ladder Root if statement and its else-if conditions.
 * @param[in] facts Binary expression facts that hold the comparisons inside the conditions.
 * @param[in] scope Lexical scope of the conditional statement.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckRepeatedConversions(const IfLadderFact& ladder, const BinaryExpressionFacts& facts, const Scope* scope,
                              DiagnosticContext& ctx);
} // namespace angel_lsp::analysis
