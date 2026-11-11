#include "analysis/resolution/CompletionReceiverResolution.h"

#include "analysis/resolution/AstRecoveryInternal.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

std::string ResolveCompletionReceiverType(IndexedByteRange receiverRange, const ExpressionTypeContext& ctx)
{
    const TSNode receiverNode = RecoverNodeAt(ctx, receiverRange.startByte, receiverRange.endByte);
    if (ts_node_is_null(receiverNode))
    {
        return "";
    }

    ExpressionTypeContext exprCtx{ctx.scope, ctx.symbolTable, ctx.sourceCode, ctx.uri};
    exprCtx.stringTypeName = ctx.symbolTable.GetStringType();
    exprCtx.arrayTypeName = ctx.symbolTable.GetDefaultArrayType();
    std::string exprType = ResolveExpressionType(receiverNode, exprCtx);
    if (!exprType.empty() && exprType != "void" && exprType != "unknown")
    {
        return exprType;
    }

    return ResolveReceiverType(receiverNode, ctx.sourceCode, ctx.symbolTable, {ctx.scope, "", ctx.uri});
}

} // namespace angel_lsp::analysis
