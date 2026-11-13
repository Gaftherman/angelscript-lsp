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
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

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
const Symbol* SelectCandidateByArgCount(std::span<const Symbol> candidateSymbols, size_t numArgs) noexcept;

/**
 * @brief Selects the candidate with the highest parameter count as a fallback.
 * @param[in] candidateSymbols Available candidate symbols.
 * @return Pointer to fallback symbol, or nullptr.
 */
const Symbol* SelectFallbackCandidate(std::span<const Symbol> candidateSymbols) noexcept;

/**
 * @brief Resolves constructor parameters for a variable direct-initialization or construct-call.
 * @param[in] declaredTypeName The type name written in the declaration or construct expression.
 * @param[in] declaratorByteOffset Byte offset where the declarator begins.
 * @param[in] ctx Semantic expression type context.
 * @param[in] args Arguments passed to constructor.
 * @return Callee resolution result matching the constructor call.
 */
ResolvedCallee ResolveConstructorParameters(std::string_view declaredTypeName, uint32_t declaratorByteOffset,
                                            const ExpressionTypeContext& ctx,
                                            std::span<const IndexedCallArgument> args);

} // namespace angel_lsp::analysis
