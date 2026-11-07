#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/index/HoverFacts.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "features/hover/HoverInternal.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
static const analysis::LocalDefinition* DefinitionAtPosition(const analysis::Scope* scope,
                                                             const lsp::Position& position)
{
    analysis::ScopeTraversalGuard cycleGuard;
    for (const analysis::Scope* current = scope; current != nullptr; current = current->parent)
    {
        if (!cycleGuard.CheckAndInsert(current))
        {
            break;
        }
        for (const analysis::LocalDefinition& def : current->definitions)
        {
            const bool afterStart = position.line > def.startLine ||
                                    (position.line == def.startLine && position.character >= def.startCharacter);
            const bool beforeEnd =
                position.line < def.endLine || (position.line == def.endLine && position.character <= def.endCharacter);

            if (afterStart && beforeEnd)
            {
                return &def;
            }
        }
    }
    return nullptr;
}

std::optional<lsp::Hover> TryHoverLocalDefinition(const HoverQueryContext& ctx)
{
    if (!ctx.rootScope || !ctx.scope)
    {
        return std::nullopt;
    }

    utils::HighResTimer symTimer;
    lsp::Position queryPos{ctx.queryLine, ctx.request.position.character};
    const analysis::LocalDefinition* def = DefinitionAtPosition(ctx.scope, queryPos);
    if (!def)
    {
        def = analysis::ResolveInScopeAt(ctx.scope, ctx.nodeText, ctx.site.targetStartByte);
    }

    if (!def ||
        (def->kind != analysis::LocalDefinitionKind::Parameter && def->kind != analysis::LocalDefinitionKind::Variable))
    {
        ctx.profiler.symMs += symTimer.ElapsedMs();
        return std::nullopt;
    }

    if (ctx.profiler.IsActive())
    {
        ctx.profiler.symbolName = def->name;
    }
    ctx.profiler.symMs += symTimer.ElapsedMs();

    utils::HighResTimer fmtTimer;
    analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri,
                                            ctx.request.nodeIndex);
    const analysis::HoverDocLookupContext docCtx = BuildDocLookupContext(ctx);
    analysis::LocalDefinitionHoverQuery query{
        .def = *def,
        .scope = ctx.scope,
        .rootScope = ctx.rootScope.get(),
        .hoverNode = {ctx.site.targetStartByte, ctx.site.targetEndByte},
        .isDirectInit = ctx.site.isDirectInitDeclarator != 0,
    };
    lsp::Hover hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                        analysis::FormatLocalDefinitionHover(query, exprCtx, docCtx)},
                     ctx.range};
    ctx.profiler.fmtMs += fmtTimer.ElapsedMs();
    return hover;
}

std::optional<lsp::Hover> TryHoverParameterAst(const HoverQueryContext& ctx)
{
    if (!ctx.site.isParameterName || ctx.site.parentStartByte >= ctx.site.parentEndByte)
    {
        return std::nullopt;
    }

    const std::string_view rawDecl = utils::TrimView(
        ctx.request.sourceCode.substr(ctx.site.parentStartByte, ctx.site.parentEndByte - ctx.site.parentStartByte));
    if (rawDecl.empty())
    {
        return std::nullopt;
    }

    return lsp::Hover{
        lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), analysis::FormatParameterAstHover(rawDecl)},
        ctx.range};
}

std::optional<lsp::Hover> TryHoverNamedArgument(const HoverQueryContext& ctx)
{
    if (!ctx.site.isNamedArgumentLabel || ctx.site.namedArgCallRange.Empty())
    {
        return std::nullopt;
    }

    analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.uri,
                                            ctx.request.nodeIndex);
    const std::vector<analysis::Symbol> candidates = analysis::ResolveNamedArgCalleeCandidates(
        ctx.site.namedArgCallRange, exprCtx, ctx.vctx.hostClass, ctx.request.config);
    lsp::MarkupContent content{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                               analysis::FormatNamedArgHover(candidates, ctx.nodeText, ctx.request.i18n)};
    if (content.value.empty())
    {
        return std::nullopt;
    }
    return lsp::Hover{std::move(content), ctx.range};
}

std::optional<lsp::Hover> TryHoverExpressionOrLocal(const HoverQueryContext& ctx)
{
    if (auto autoHover = TryHoverAutoType(ctx))
    {
        return autoHover;
    }

    if (ctx.site.isMemberOfExpression)
    {
        if (auto memberHover = TryHoverMemberAccess(ctx))
        {
            return memberHover;
        }
    }

    if (auto localHover = TryHoverLocalDefinition(ctx))
    {
        return localHover;
    }

    if (auto paramHover = TryHoverParameterAst(ctx))
    {
        return paramHover;
    }

    if (auto namedArgHover = TryHoverNamedArgument(ctx))
    {
        return namedArgHover;
    }

    return std::nullopt;
}
} // namespace angel_lsp::features
