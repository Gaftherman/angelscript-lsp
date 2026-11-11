#include "analysis/resolution/CodeActionResolution.h"

#include "analysis/resolution/AstRecoveryInternal.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

std::string ResolveExpressionTypeAt(IndexedByteRange expressionRange, const ExpressionTypeContext& ctx)
{
    const TSNode node = RecoverNodeAt(ctx, expressionRange.startByte, expressionRange.endByte);
    if (ts_node_is_null(node))
    {
        return "";
    }
    const ExpressionTypeContext exprCtx{ctx.scope, ctx.symbolTable, ctx.sourceCode, ctx.uri};
    return ResolveExpressionType(node, exprCtx);
}

} // namespace angel_lsp::analysis
