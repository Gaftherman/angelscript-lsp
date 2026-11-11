#pragma once

#include "analysis/index/InlayHintFacts.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <cstdint>
#include <string>

namespace angel_lsp::analysis
{

/**
 * @brief Deduces the type of an expression located at [startByte, endByte).
 * @param[in] startByte Start byte offset of the expression.
 * @param[in] endByte End byte offset of the expression.
 * @param[in] ctx Semantic expression type context.
 * @return Deduced type string, or empty string if unresolvable.
 */
std::string DeduceExpressionTypeAt(uint32_t startByte, uint32_t endByte, const ExpressionTypeContext& ctx);

/**
 * @brief Deduces the return type of a lambda body at the specified lambda site.
 * @param[in] site Indexed lambda site facts.
 * @param[in] ctx Semantic expression type context.
 * @return Deduced return type string, or empty string if unresolvable.
 */
std::string DeduceLambdaReturnTypeAt(const IndexedLambdaSite& site, const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
