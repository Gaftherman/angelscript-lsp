#pragma once

#include "analysis/DiagnosticContext.h"
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
struct Scope;

/**
 * @brief Checks for repeated implicit conversions in conditional statement ladders.
 * @param[in] rootIfNode The root if_statement syntax node.
 * @param[in] scope Lexical scope of the conditional statement.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckRepeatedConversions(TSNode rootIfNode, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Finds all AST node occurrences of an expression text within an if statement ladder's conditions.
 * @param[in] rootIf Root if_statement node.
 * @param[in] exprText Target expression text to find.
 * @param[in] sourceCode Source document content.
 * @return Vector of matching AST nodes.
 */
std::vector<TSNode> FindOccurrencesInIfLadder(TSNode rootIf, std::string_view exprText,
                                              std::string_view sourceCode);
} // namespace angel_lsp::analysis
