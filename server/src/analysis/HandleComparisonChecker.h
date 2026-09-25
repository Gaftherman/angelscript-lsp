#pragma once

#include "analysis/DiagnosticContext.h"
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
struct Scope;

/**
 * @brief Validates handle equality and relational operations in binary expressions.
 * @param[in] node Binary expression syntax node.
 * @param[in] scope Lexical scope enclosing the expression.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckHandleComparison(TSNode node, const Scope* scope, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
