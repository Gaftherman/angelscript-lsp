#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/checkers/LValueChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Validates whether a call expression target is a modifiable reference l-value.
 * @param[in] callNode Call expression AST node.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in] scope Lexical scope active at the call site.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckCallLValue(TSNode callNode, const AstCheckContext& request, const Scope* scope, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
