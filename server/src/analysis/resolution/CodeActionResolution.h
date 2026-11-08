#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <string>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves the type of the expression written in a byte range.
 * @param[in] expressionRange Byte range of the expression to resolve.
 * @param[in] ctx Semantic expression context driving type resolution.
 * @pre ctx.nodeIndex is set; the node is recovered from it.
 * @return Resolved type spelling, or an empty string when no node covers the range.
 * @note Cost one RecoverNodeAt descent plus ResolveExpressionType.
 */
[[nodiscard]] std::string ResolveExpressionTypeAt(IndexedByteRange expressionRange, const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
