#include "features/hover/HoverHandler.h"

#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/index/HoverFacts.h"
#include "analysis/index/ScopeTree.h"
#include "features/hover/HoverInternal.h"
#include "utils/LspLogger.h"
#include "utils/MultiFileLogger.h"
#include "utils/Timer.h"

#include <lsp/types.h>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string_view>
#include <utility>

namespace angel_lsp::features
{
bool HoverProfiler::IsActive() const noexcept
{
    return m_logger != nullptr || utils::MultiFileLogger::Instance().IsInitialized();
}

void HoverProfiler::Emit()
{
    if (emitted)
    {
        return;
    }
    emitted = true;
    double totalMs = totalTimer.ElapsedMs();
    if (m_logger)
    {
        m_logger->LogInfo(
            fmt::format("[Hover Profile] Total: {:.2f} ms (NodeLookup: {:.2f} ms, SymbolResolve: {:.2f} ms, "
                        "Formatting: {:.2f} ms) at {}:{}",
                        totalMs, nodeMs, symMs, fmtMs, line, character));
    }
    if (utils::MultiFileLogger::Instance().IsInitialized())
    {
        utils::MultiFileLogger::Instance().LogHover(
            utils::MultiFileLogLevel::Info,
            fmt::format("Pos({}:{}) Node='{}' Path='{}' Symbol='{}' [NodeLookup={:.2f}ms SymbolResolve={:.2f}ms "
                        "Formatting={:.2f}ms Total={:.2f}ms]",
                        line, character, nodeType, pathText, symbolName, nodeMs, symMs, fmtMs, totalMs),
            totalMs);
    }
}

HoverProfiler::~HoverProfiler()
{
    Emit();
}

static std::optional<lsp::Hover> HoverIncludeDirective(const HoverRequest& request)
{
    if (request.sourceCode.find('#') == std::string_view::npos)
    {
        return std::nullopt;
    }

    auto directive =
        analysis::FindIncludeDirective(request.sourceCode, request.position.line, request.position.character);
    if (!directive)
    {
        return std::nullopt;
    }

    const std::string_view rawPath =
        request.sourceCode.substr(directive->pathStartByte, directive->pathEndByte - directive->pathStartByte);

    lsp::Range range{};
    range.start.line = request.position.line;
    range.start.character = static_cast<lsp::uint>(directive->hash);
    range.end.line = request.position.line;
    range.end.character = static_cast<lsp::uint>(directive->closeQuote + 1);

    return lsp::Hover{lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                         analysis::FormatIncludeDirectiveHover(rawPath, request.sources, request.i18n)},
                      range};
}

analysis::HoverDocLookupContext BuildDocLookupContext(const HoverQueryContext& ctx)
{
    return analysis::HoverDocLookupContext{.sources = &ctx.request.sources,
                                           .documentUri = ctx.request.uri,
                                           .sourceCode = ctx.request.sourceCode,
                                           .symbolTable = &ctx.request.symbolTable,
                                           .config = ctx.request.config,
                                           .i18n = ctx.request.i18n};
}

static HoverQueryContext BuildHoverQueryContext(const HoverRequest& request, HoverProfiler& profiler,
                                                const analysis::IndexedHoverSite& site, std::string_view nodeText,
                                                const lsp::Range& range)
{
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const uint32_t queryLine = request.position.line;
    const analysis::Scope* scope =
        rootScope ? analysis::FindInnermostScope(rootScope.get(), queryLine, request.position.character) : nullptr;

    return HoverQueryContext{request, profiler, site, nodeText, range, std::move(rootScope), queryLine, scope};
}

std::optional<lsp::Hover> GetHover(const HoverRequest& request)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    HoverProfiler profiler;
    profiler.m_logger = request.logger;
    profiler.line = request.position.line;
    profiler.character = request.position.character;

    if (auto includeHover = HoverIncludeDirective(request))
    {
        profiler.nodeType = "include";
        profiler.pathText = "include";
        profiler.symbolName = "include";
        return includeHover;
    }

    utils::HighResTimer nodeTimer;
    auto site = analysis::FindHoverSite(*request.nodeIndex, request.sourceCode, request.position.line,
                                        request.position.character);
    profiler.nodeMs = nodeTimer.ElapsedMs();
    if (!site)
    {
        return std::nullopt;
    }

    const std::string_view nodeText =
        request.sourceCode.substr(site->targetStartByte, site->targetEndByte - site->targetStartByte);
    lsp::Range range{{site->startLine, site->startColumn}, {site->endLine, site->endColumn}};

    if (profiler.IsActive())
    {
        profiler.nodeType = "identifier";
        profiler.pathText = nodeText;
        profiler.symbolName = nodeText;
    }

    HoverQueryContext ctx = BuildHoverQueryContext(request, profiler, *site, nodeText, range);

    if (auto earlyHover = TryHoverLiteralOrType(ctx))
    {
        return earlyHover;
    }

    if (auto thisHover = TryHoverThis(ctx))
    {
        return thisHover;
    }

    if (auto lambdaHover = TryHoverLambda(ctx))
    {
        return lambdaHover;
    }

    if (auto exprHover = TryHoverExpressionOrLocal(ctx))
    {
        return exprHover;
    }

    return TryHoverSymbolCandidates(ctx);
}
} // namespace angel_lsp::features
