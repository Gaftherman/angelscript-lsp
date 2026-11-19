#include "analysis/resolution/CompletionReceiverResolution.h"

#include "analysis/resolution/ExpressionTypeResolver.h"
#include "parser/Primitives.h"

namespace angel_lsp::analysis
{

std::string ResolveCompletionReceiverType(IndexedByteRange receiverRange, const ExpressionTypeContext& ctx)
{
    ExpressionTypeContext exprCtx{ctx.scope, ctx.symbolTable, ctx.sourceCode, ctx.uri, ctx.nodeIndex};
    exprCtx.stringTypeName = ctx.symbolTable.GetStringType();
    exprCtx.arrayTypeName = ctx.symbolTable.GetDefaultArrayType();
    std::string exprType = ResolveExpressionTypeAt(receiverRange, exprCtx);
    if (!exprType.empty() && !parser::primitives::IsVoidType(exprType) && exprType != "unknown")
    {
        return exprType;
    }

    ReceiverTypeContext recvCtx{ctx.scope, "", ctx.uri};
    recvCtx.nodeIndex = ctx.nodeIndex;
    std::string recvType = ResolveReceiverType(receiverRange, ctx.sourceCode, ctx.symbolTable, recvCtx);
    if (!recvType.empty() && !parser::primitives::IsVoidType(recvType) && recvType != "unknown")
    {
        return recvType;
    }
    return "";
}

} // namespace angel_lsp::analysis
