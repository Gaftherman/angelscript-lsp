#include "analysis/resolution/CompletionReceiverResolution.h"

#include "analysis/resolution/ExpressionTypeResolver.h"

namespace angel_lsp::analysis
{

std::string ResolveCompletionReceiverType(IndexedByteRange receiverRange, const ExpressionTypeContext& ctx)
{
    ExpressionTypeContext exprCtx{ctx.scope, ctx.symbolTable, ctx.sourceCode, ctx.uri, ctx.nodeIndex};
    exprCtx.stringTypeName = ctx.symbolTable.GetStringType();
    exprCtx.arrayTypeName = ctx.symbolTable.GetDefaultArrayType();
    std::string exprType = ResolveExpressionTypeAt(receiverRange, exprCtx);
    if (!exprType.empty() && exprType != "void" && exprType != "unknown" &&
        exprType.find("<poison>") == std::string::npos)
    {
        return exprType;
    }

    ReceiverTypeContext recvCtx{ctx.scope, "", ctx.uri};
    recvCtx.nodeIndex = ctx.nodeIndex;
    std::string recvType = ResolveReceiverType(receiverRange, ctx.sourceCode, ctx.symbolTable, recvCtx);
    if (!recvType.empty() && recvType != "void" && recvType != "unknown" &&
        recvType.find("<poison>") == std::string::npos)
    {
        return recvType;
    }
    return "";
}

} // namespace angel_lsp::analysis
