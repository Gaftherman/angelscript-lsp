#include "analysis/resolution/HoverResolution.h"
#include "features/hover/HoverInternal.h"
#include "utils/Timer.h"

#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

std::optional<lsp::Hover> TryHoverMemberAccess(const HoverQueryContext& ctx)
{
    utils::HighResTimer symTimer;
    if (!ctx.site.isMemberOfExpression || ctx.site.receiverRange.Empty())
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri,
                                            ctx.request.nodeIndex);
    auto res = analysis::ResolveMemberAccess(ctx.site.receiverRange, ctx.nodeText, exprCtx, ctx.request.config);
    if (!res)
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    if (ctx.profiler.IsActive())
    {
        ctx.profiler.pathText = ctx.nodeText;
    }
    ctx.profiler.symMs += symTimer.ElapsedMs();
    return FormatSymbolsHover(res->symbols, res->accessorPropertyType, ctx);
}
} // namespace angel_lsp::features
