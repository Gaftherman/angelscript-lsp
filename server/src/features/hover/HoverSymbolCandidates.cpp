#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/index/HoverFacts.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "features/hover/HoverInternal.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
std::optional<analysis::Symbol> ResolveOrMatchCandidate(std::vector<analysis::Symbol>& symbols,
                                                        const HoverQueryContext& ctx)
{
    analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri,
                                            ctx.request.nodeIndex);
    auto best = analysis::ResolveCallOverload(ctx.site.targetStartByte, ctx.site.targetEndByte, symbols, exprCtx);
    if (!best)
    {
        auto it = std::find_if(symbols.begin(), symbols.end(),
                               [&](const analysis::Symbol& s)
                               {
                                   if (!utils::CaseInsensitiveEquals(s.fileUri, ctx.request.uri))
                                   {
                                       return false;
                                   }
                                   return s.selectionRange.startLine == ctx.queryLine || s.startLine == ctx.queryLine;
                               });
        if (it == symbols.end())
        {
            it = std::find_if(symbols.begin(), symbols.end(),
                              [&](const analysis::Symbol& s)
                              {
                                  if (!utils::CaseInsensitiveEquals(s.fileUri, ctx.request.uri))
                                  {
                                      return false;
                                  }
                                  return ctx.queryLine >= s.fullRange.startLine && ctx.queryLine <= s.fullRange.endLine;
                              });
        }
        if (it != symbols.end())
        {
            best = *it;
            std::rotate(symbols.begin(), it, it + 1);
        }
        else if (!symbols.empty())
        {
            best = symbols.front();
        }
    }
    else
    {
        auto it = std::find_if(symbols.begin(), symbols.end(), [&](const analysis::Symbol& s)
                               { return s.name == best->name && analysis::HasSameParameterList(s, *best); });
        if (it != symbols.end())
        {
            std::rotate(symbols.begin(), it, it + 1);
        }
    }
    return best;
}

void PromoteMatchedCandidate(std::vector<analysis::Symbol>& symbols, std::optional<analysis::Symbol>& best)
{
    if (!best || symbols.empty())
    {
        return;
    }
    auto it = std::find_if(symbols.begin(), symbols.end(), [&](const analysis::Symbol& s)
                           { return s.name == best->name && analysis::HasSameParameterList(s, *best); });
    if (it != symbols.end())
    {
        std::rotate(symbols.begin(), it, it + 1);
        best = symbols.front();
    }
}

lsp::Hover FormatSymbolsHover(std::vector<analysis::Symbol>& symbols, std::string_view accessorPropertyType,
                              const HoverQueryContext& ctx)
{
    auto best = ResolveOrMatchCandidate(symbols, ctx);
    analysis::RemoveDuplicateSymbols(symbols);
    PromoteMatchedCandidate(symbols, best);
    utils::HighResTimer fmtTimer;
    analysis::HoverPeekContext peekCtx{.uri = ctx.request.uri,
                                       .line = ctx.request.position.line,
                                       .character = ctx.request.position.character,
                                       .i18n = ctx.request.i18n};
    const analysis::HoverDocLookupContext docCtx = BuildDocLookupContext(ctx);
    lsp::Hover hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                        analysis::FormatSymbolsMarkdown(symbols, best, accessorPropertyType,
                                                                        ctx.nodeText, docCtx, peekCtx)},
                     ctx.range};
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return hover;
}

std::optional<lsp::Hover> TryHoverScopeFallback(const HoverQueryContext& ctx)
{
    if (!ctx.scope)
    {
        return std::nullopt;
    }

    const analysis::LocalDefinition* def =
        analysis::ResolveInScopeAt(ctx.scope, ctx.nodeText, ctx.site.targetStartByte);
    if (!def)
    {
        return std::nullopt;
    }

    utils::HighResTimer fmtTimer;
    lsp::Hover hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                        analysis::FormatScopeFallbackHover(*def, ctx.request.sourceCode)},
                     ctx.range};
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return hover;
}

std::optional<lsp::Hover> TryHoverSymbolCandidates(HoverQueryContext& ctx)
{
    utils::HighResTimer symTimer;
    std::string_view pathText;
    analysis::ScopedSymbolQuery query{
        .site = ctx.site,
        .nodeText = ctx.nodeText,
        .sourceCode = ctx.request.sourceCode,
        .symbolTable = ctx.request.symbolTable,
        .nodeIndex = ctx.request.nodeIndex,
    };
    analysis::HoverCandidateSymbols candidates{analysis::CollectHoverScopedSymbols(query, pathText), {}};
    std::vector<analysis::Symbol>& symbols = candidates.symbols;
    if (!pathText.empty() && ctx.profiler.IsActive())
    {
        ctx.profiler.pathText = pathText;
    }

    if (!ctx.site.isGlobalScopeQualified && !ctx.site.isQualifiedMember)
    {
        analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode,
                                                ctx.request.uri, ctx.request.nodeIndex);
        analysis::AppendEnclosingClassMethods(
            analysis::IndexedByteRange{ctx.site.targetStartByte, ctx.site.targetEndByte}, ctx.nodeText, exprCtx,
            symbols);
    }
    DisambiguateContainerSymbols(ctx, symbols);
    DisambiguateTypeContextSymbols(ctx, symbols);
    AppendConstructorSymbolsIfCall(ctx, symbols);

    analysis::VirtualHostSymbolQuery vquery{
        .targetRange = {ctx.site.targetStartByte, ctx.site.targetEndByte},
        .nodeText = ctx.nodeText,
        .virtualHostClass = ctx.vctx.isVirtual ? std::string_view(ctx.vctx.hostClass) : std::string_view{},
        .sourceCode = ctx.request.sourceCode,
        .fileUri = ctx.request.uri,
        .nodeIndex = ctx.request.nodeIndex,
        .symbolTable = ctx.request.symbolTable,
    };

    if (!ctx.site.isGlobalScopeQualified)
    {
        analysis::AppendVirtualHostSymbols(vquery, candidates);
    }

    FilterDirectRootGlobalSymbols(symbols, ctx.site.isDirectRootGlobal != 0);

    if (symbols.empty() && ctx.rootScope && !ctx.site.isGlobalScopeQualified)
    {
        if (auto fallbackHover = TryHoverScopeFallback(ctx))
        {
            ctx.profiler.symMs += symTimer.ElapsedMs();
            return fallbackHover;
        }
    }

    if (symbols.empty() && candidates.accessorPropertyType.empty() && !ctx.site.isGlobalScopeQualified)
    {
        const int accessorMode = ctx.request.config ? ctx.request.config->engine.propertyAccessorMode : 2;
        if (accessorMode >= 2)
        {
            symbols = analysis::FindGlobalPropertyAccessors(ctx.nodeText, ctx.request.symbolTable, accessorMode == 3);
            candidates.accessorPropertyType = analysis::PropertyTypeFromAccessors(symbols);
        }
    }

    if (symbols.empty() && !ctx.site.isGlobalScopeQualified)
    {
        lsp::MarkupContent special{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                   analysis::TryHoverDelegateProperty(vquery, ctx.request.config)};
        if (!special.value.empty())
        {
            return lsp::Hover{std::move(special), ctx.range};
        }
    }

    ctx.profiler.symMs += symTimer.ElapsedMs();
    if (symbols.empty())
    {
        return std::nullopt;
    }

    return FormatSymbolsHover(symbols, candidates.accessorPropertyType, ctx);
}
} // namespace angel_lsp::features
