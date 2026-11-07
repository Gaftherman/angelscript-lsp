#pragma once

#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/InlayHintFacts.h"
#include "analysis/index/Symbol.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/InlayCalleeResolution.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Recovers a Tree-Sitter AST node matching an exact byte interval [startByte, endByte).
 * @param[in] ctx Semantic context containing NodeIndex with document AST root.
 * @param[in] startByte Inclusive start byte offset.
 * @param[in] endByte Exclusive end byte offset.
 * @return Recovered AST node, or null node if recovery fails.
 */
TSNode RecoverNodeAt(const ExpressionTypeContext& ctx, uint32_t startByte, uint32_t endByte) noexcept;

/**
 * @brief Retrieves parameter information vector pointer from a Function or Funcdef symbol if available.
 * @param[in] sym Symbol to inspect.
 * @return Pointer to vector of parameter information, or nullptr if not callable.
 */
const std::vector<ParameterInformation>* GetParametersIfCallable(const Symbol& sym) noexcept;

/**
 * @brief Counts the minimum number of arguments required by a callable's parameters.
 * @param[in] parameters Vector of parameter information to evaluate.
 * @return Count of non-default, non-vararg parameters.
 */
size_t CountRequiredParameters(const std::vector<ParameterInformation>& parameters) noexcept;

/**
 * @brief Extracts callee result with URI, name, and parameters from a resolved callable symbol.
 * @param[in] sym Resolved symbol.
 * @return Callee resolution result.
 */
ResolvedCallee ExtractCalleeFromSymbol(const Symbol* sym);

/**
 * @brief Selects the candidate matching the given argument count and default parameter bounds.
 * @param[in] candidateSymbols Available candidate symbols.
 * @param[in] numArgs Number of arguments passed.
 * @return Pointer to best matching candidate symbol, or nullptr.
 */
const Symbol* SelectCandidateByArgCount(const std::vector<Symbol>& candidateSymbols, size_t numArgs) noexcept;

/**
 * @brief Selects the candidate with the highest parameter count as a fallback.
 * @param[in] candidateSymbols Available candidate symbols.
 * @return Pointer to fallback symbol, or nullptr.
 */
const Symbol* SelectFallbackCandidate(const std::vector<Symbol>& candidateSymbols) noexcept;

/**
 * @brief Resolves constructor parameters for a variable direct-initialization or construct-call.
 * @param[in] declaredTypeName The type name written in the declaration or construct expression.
 * @param[in] declaratorNode The variable_declarator or construct_call AST node.
 * @param[in] ctx Semantic expression type context.
 * @param[in] args Arguments passed to constructor.
 * @return Callee resolution result matching the constructor call.
 */
ResolvedCallee ResolveConstructorParameters(std::string_view declaredTypeName, TSNode declaratorNode,
                                            const ExpressionTypeContext& ctx,
                                            std::span<const IndexedCallArgument> args);

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
