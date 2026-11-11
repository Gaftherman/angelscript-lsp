#pragma once

#include "analysis/index/InlayHintFacts.h"
#include "analysis/index/Symbol.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Collects candidate method symbols for a member call site across type hierarchy.
 * @param[in] site Call site fact record.
 * @param[in] ctx Semantic expression type context.
 * @return Vector of candidate symbols.
 */
std::vector<Symbol> CollectMemberCalleeCandidates(const IndexedCallSite& site, const ExpressionTypeContext& ctx);

/**
 * @brief Collects candidate member methods from enclosing class hierarchy for an unqualified call.
 * @param[in] calleeName Callee identifier text.
 * @param[in] callByteOffset Byte offset of the call site.
 * @param[in] ctx Semantic expression type context.
 * @param[in,out] candidateSymbols Symbol collection receiving candidates.
 */
void CollectEnclosingClassCallees(const std::string& calleeName, uint32_t callByteOffset,
                                  const ExpressionTypeContext& ctx, std::vector<Symbol>& candidateSymbols);

/**
 * @brief Resolves scoped callee candidates by walking enclosing namespaces and inherited hierarchies.
 * @param[in] calleeName Callee identifier string.
 * @param[in] callByteOffset Byte offset of the call site.
 * @param[in] ctx Semantic expression type context.
 * @return Vector of resolved candidate symbols.
 */
std::vector<Symbol> CollectScopedCalleeCandidates(const std::string& calleeName, uint32_t callByteOffset,
                                                  const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
