#include "features/inlay_hint/InlayHintHandler.h"

#include "analysis/index/InlayHintFacts.h"
#include "features/inlay_hint/InlayHintInternal.h"
#include "utils/LspLogger.h"

#include <algorithm>
#include <optional>
#include <span>
#include <spdlog/fmt/fmt.h>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Iteratively collects inlay hints by traversing indexed syntactic sites across the document range.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Inlay hints vector receiving collected items.
 */
static void CollectInlayHints(const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return;
    }

    auto sites = analysis::CollectInlayHintSites(*request.nodeIndex, request.range.start.line, request.range.end.line,
                                                 request.sourceCode);

    for (const auto& call : sites.calls)
    {
        auto argsSpan = std::span(sites.callArguments.data() + call.firstArgument, call.argumentCount);
        ProcessCallSite(call, argsSpan, request, hints);
    }
    for (const auto& autoVar : sites.autoVars)
    {
        ProcessAutoVariable(autoVar, request, hints);
    }
    for (const auto& ctorInit : sites.ctorInits)
    {
        auto argsSpan = std::span(sites.callArguments.data() + ctorInit.firstArgument, ctorInit.argumentCount);
        ProcessConstructorInit(ctorInit, argsSpan, request, hints);
    }
    for (const auto& lambda : sites.lambdas)
    {
        ProcessLambda(lambda, request, hints);
    }
    for (const auto& enumDecl : sites.enumDeclarations)
    {
        auto valuesSpan = std::span(sites.enumValues.data() + enumDecl.firstValue, enumDecl.valueCount);
        ProcessEnum(valuesSpan, request, hints);
    }
}

std::optional<InlayHintResult> GetInlayHints(const InlayHintRequest& request)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[InlayHint] Computing inlay hints for URI: {}", request.uri));
    }

    std::vector<lsp::InlayHint> hints;
    CollectInlayHints(request, hints);

    std::stable_sort(hints.begin(), hints.end(),
                     [](const lsp::InlayHint& a, const lsp::InlayHint& b) noexcept
                     {
                         if (a.position.line != b.position.line)
                         {
                             return a.position.line < b.position.line;
                         }
                         return a.position.character < b.position.character;
                     });

    if (request.logger && request.logger->IsTraceEnabled())
    {
        request.logger->LogTrace(fmt::format("[InlayHint] Computed {} hints for URI: {}", hints.size(), request.uri));
    }

    return hints;
}

} // namespace angel_lsp::features
