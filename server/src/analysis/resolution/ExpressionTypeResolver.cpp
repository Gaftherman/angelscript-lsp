#include "analysis/resolution/ExpressionTypeResolver.h"

#include "analysis/index/ExpressionTypeASTRecovery.h"

#include <string>

namespace angel_lsp::analysis
{

std::string ResolveExpressionTypeAt(IndexedByteRange expressionRange, const ExpressionTypeContext& ctx)
{
    return RecoverExpressionTypeFromAST(expressionRange, ctx);
}

} // namespace angel_lsp::analysis
