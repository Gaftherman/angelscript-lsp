#pragma once

#include "analysis/index/Symbol.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Collects candidate method symbols for a member call expression across type hierarchy.
 * @param[in] funcNode AST member_expression node.
 * @param[in] ctx Semantic expression type context.
 * @return Vector of candidate symbols.
 */
std::vector<Symbol> CollectMemberCalleeCandidates(TSNode funcNode, const ExpressionTypeContext& ctx);

/**
 * @brief Collects candidate member methods from enclosing class hierarchy for an unqualified call.
 * @param[in] calleeName Callee identifier text.
 * @param[in] callNode AST call_expression node.
 * @param[in] ctx Semantic expression type context.
 * @param[in,out] candidateSymbols Symbol collection receiving candidates.
 */
void CollectEnclosingClassCallees(const std::string& calleeName, TSNode callNode, const ExpressionTypeContext& ctx,
                                  std::vector<Symbol>& candidateSymbols);

/**
 * @brief Resolves scoped callee candidates by walking enclosing namespaces and inherited hierarchies.
 * @param[in] calleeName Callee identifier string.
 * @param[in] callNode AST call_expression node.
 * @param[in] ctx Semantic expression type context.
 * @return Vector of resolved candidate symbols.
 */
std::vector<Symbol> CollectScopedCalleeCandidates(const std::string& calleeName, TSNode callNode,
                                                  const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
