#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/index/HoverFacts.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "features/hover/HoverInternal.h"
#include "i18n/i18n.h"
#include "utils/Timer.h"

#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <utility>

namespace angel_lsp::features
{
std::optional<lsp::Hover> TryHoverPrimitiveType(std::string_view nodeText, const lsp::Range& range,
                                                const angel_lsp::i18n::I18n* i18n, HoverProfiler& profiler)
{
    if (!analysis::IsPrimitiveTypeName(nodeText) || nodeText == "auto")
    {
        return std::nullopt;
    }
    profiler.nodeType = "primitive_type";
    if (profiler.IsActive())
    {
        profiler.pathText = nodeText;
        profiler.symbolName = nodeText;
    }
    utils::HighResTimer fmtTimer;
    lsp::Hover hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                        analysis::FormatPrimitiveTypeHover(nodeText, i18n)},
                     range};
    profiler.fmtMs += fmtTimer.ElapsedMs();
    return hover;
}

std::optional<lsp::Hover> TryHoverWildcardType(std::string_view nodeText, const lsp::Range& range,
                                               const angel_lsp::i18n::I18n* i18n, HoverProfiler& profiler)
{
    if (nodeText != "?")
    {
        return std::nullopt;
    }
    profiler.nodeType = "wildcard_type";
    if (profiler.IsActive())
    {
        profiler.pathText = "?";
        profiler.symbolName = "?";
    }
    utils::HighResTimer fmtTimer;
    lsp::Hover hover{
        lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), analysis::FormatWildcardTypeHover(i18n)},
        range};
    profiler.fmtMs += fmtTimer.ElapsedMs();
    return hover;
}

std::optional<lsp::Hover> TryHoverThis(const HoverQueryContext& ctx)
{
    if (ctx.nodeText != "this")
    {
        return std::nullopt;
    }

    std::vector<analysis::ContainerInfo> containers;
    std::string_view className;
    if (ctx.request.nodeIndex)
    {
        containers =
            ctx.request.nodeIndex->CollectEnclosingContainers(ctx.site.targetStartByte, ctx.request.sourceCode);
        for (const auto& c : containers)
        {
            if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
            {
                className = c.name;
                break;
            }
        }
    }

    if (className.empty())
    {
        return std::nullopt;
    }

    ctx.profiler.nodeType = "this";
    if (ctx.profiler.IsActive())
    {
        ctx.profiler.pathText = "this";
        ctx.profiler.symbolName = "this";
    }

    utils::HighResTimer fmtTimer;
    lsp::Hover hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                        analysis::FormatThisHover(className, ctx.nodeText)},
                     ctx.range};
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return hover;
}

std::optional<lsp::Hover> TryHoverLambda(const HoverQueryContext& ctx)
{
    if (ctx.nodeText != "function" || ctx.site.enclosingLambdaRange.Empty())
    {
        return std::nullopt;
    }

    utils::HighResTimer fmtTimer;
    analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri,
                                            ctx.request.nodeIndex);
    const analysis::HoverDocLookupContext docCtx = BuildDocLookupContext(ctx);
    lsp::Hover hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                        analysis::FormatLambdaHover(ctx.site.enclosingLambdaRange, exprCtx, docCtx)},
                     ctx.range};
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return hover;
}

std::optional<lsp::Hover> TryHoverAutoType(const HoverQueryContext& ctx)
{
    if (ctx.nodeText != "auto" && ctx.nodeText != "auto@")
    {
        return std::nullopt;
    }
    if (ctx.site.enclosingAutoDeclRange.Empty())
    {
        return std::nullopt;
    }

    analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri,
                                            ctx.request.nodeIndex);
    lsp::MarkupContent content{
        lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
        analysis::ResolveAutoHoverMarkdown(ctx.site.enclosingAutoDeclRange, ctx.site.isAutoHandle != 0, exprCtx)};
    if (content.value.empty())
    {
        return std::nullopt;
    }
    return lsp::Hover{std::move(content), ctx.range};
}

std::optional<lsp::Hover> TryHoverConcatenatedString(const HoverQueryContext& ctx, bool showLength)
{
    if (ctx.site.topmostBinaryPlusRange.Empty())
    {
        return std::nullopt;
    }

    const auto rootScope = ctx.request.scopeIndex.GetRoot(ctx.request.uri);
    const analysis::Scope* scope =
        rootScope ? analysis::FindInnermostScope(rootScope.get(), ctx.site.startLine, ctx.site.startColumn) : nullptr;
    analysis::ExpressionTypeContext exprCtx(scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri,
                                            ctx.request.nodeIndex);
    lsp::MarkupContent content{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                               analysis::FormatConcatenatedPathHover(ctx.site.topmostBinaryPlusRange, exprCtx,
                                                                     showLength, BuildDocLookupContext(ctx))};
    if (content.value.empty())
    {
        return std::nullopt;
    }
    return lsp::Hover{std::move(content), ctx.range};
}

std::optional<lsp::Hover> TryHoverStringLiteral(const HoverQueryContext& ctx)
{
    const bool showLength = !ctx.request.config || ctx.request.config->features.hoverStringLiteralLength;
    const bool enablePathRes = !ctx.request.config || ctx.request.config->features.hoverStringLiteralPathResolution;

    if (enablePathRes)
    {
        if (auto concatHover = TryHoverConcatenatedString(ctx, showLength))
        {
            return concatHover;
        }
    }

    if (ctx.site.kind == analysis::HoverNodeKind::ConcatenatedString)
    {
        return std::nullopt;
    }

    const std::string_view txt = ctx.nodeText;
    const std::string_view unquoted = parser::UnquoteStringView(txt);

    if (enablePathRes)
    {
        lsp::MarkupContent asset{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                 analysis::FormatStringPathHover(unquoted, showLength, BuildDocLookupContext(ctx))};
        if (!asset.value.empty())
        {
            return lsp::Hover{std::move(asset), ctx.range};
        }
    }

    return lsp::Hover{
        lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                           analysis::FormatStringLiteralHover(txt, unquoted, showLength, ctx.request.i18n)},
        ctx.range};
}

std::optional<lsp::Hover> TryHoverLiteralOrType(const HoverQueryContext& ctx)
{
    if (ctx.site.kind == analysis::HoverNodeKind::StringLiteral ||
        ctx.site.kind == analysis::HoverNodeKind::ConcatenatedString)
    {
        if (auto strHover = TryHoverStringLiteral(ctx))
        {
            return strHover;
        }
    }

    if (auto primHover = TryHoverPrimitiveType(ctx.nodeText, ctx.range, ctx.request.i18n, ctx.profiler))
    {
        return primHover;
    }

    return TryHoverWildcardType(ctx.nodeText, ctx.range, ctx.request.i18n, ctx.profiler);
}
} // namespace angel_lsp::features
