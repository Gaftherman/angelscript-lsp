#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <string>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves expression type by recovering and traversing AST syntax.
 * @param[in] expressionRange Byte span of the expression to resolve.
 * @param[in] ctx Semantic expression type resolution context.
 * @return Resolved type name or empty string.
 */
std::string RecoverExpressionTypeFromAST(IndexedByteRange expressionRange, const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
