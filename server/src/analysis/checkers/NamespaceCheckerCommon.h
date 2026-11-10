#pragma once

#include "analysis/checkers/NamespaceChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/SymbolTable.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a scoped identifier refers to a known namespace or type prefix.
 * @param[in] node AST node representing the scoped identifier.
 * @param[in] sourceCode Document source text.
 * @param[in] table Workspace symbol table used to resolve prefix candidates.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckScopedIdentifier(TSNode node, std::string_view sourceCode, const SymbolTable& table, DiagnosticContext& ctx);

/**
 * @brief Checks if an unqualified function call resolves to an in-scope symbol.
 * @param[in] node AST node representing the call expression.
 * @param[in] request Contextual request bundle containing AST root, source text, and node index.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckCallExpression(TSNode node, const NamespaceCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Checks if an import declaration mistakenly specifies a body block.
 * @param[in] node AST node representing the import declaration or error node.
 * @param[in] sourceCode Document source text.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckImportBody(TSNode node, std::string_view sourceCode, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
