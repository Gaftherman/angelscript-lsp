#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "parser/SyntaxTree.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <chrono>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{
static bool AreSemanticTokensEditsValid(const std::vector<lsp::SemanticTokensEdit>& edits)
{
    for (const auto& edit : edits)
    {
        if (edit.start % 5 != 0 || edit.deleteCount % 5 != 0 || (edit.data.has_value() && edit.data->size() % 5 != 0))
        {
            return false;
        }
    }
    return true;
}

lsp::requests::TextDocument_SemanticTokens_Full::Result
Server::HandleRequestsTextDocument_SemanticTokens_Full(lsp::requests::TextDocument_SemanticTokens_Full::Params&& req)
{
    if (!m_config.features.enableSemanticTokens)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    utils::HighResTimer timer;
    auto tokens = ComputeAndCacheSemanticTokens(doc->uri, *doc->text);
    const double elapsedMs = timer.ElapsedMs();
    LogInfo(fmt::format("[Semantic Tokens] Finished full in {:.2f} ms for {} ({} tokens)", elapsedMs, doc->uri,
                        tokens.data.size() / 5));
    return tokens;
}

lsp::requests::TextDocument_SemanticTokens_Full_Delta::Result
Server::HandleRequestsTextDocument_SemanticTokens_Full_Delta(
    lsp::requests::TextDocument_SemanticTokens_Full_Delta::Params&& req)
{
    if (!m_config.features.enableSemanticTokens)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    utils::HighResTimer timer;
    CachedTokensSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(m_semanticTokensMutex);
        if (const auto cached = m_semanticTokensCache.find(doc->uri);
            cached != m_semanticTokensCache.end() && cached->second.resultId == req.previousResultId)
        {
            snap.canDiff = true;
            snap.prevHadError = cached->second.hasError;
            snap.cachedVersion = cached->second.version;
            snap.previous = std::move(cached->second.data);
        }
    }

    const bool currHasError = doc->docHandle && parser::TreeHasError(doc->docHandle->tree);
    if (!snap.canDiff || snap.prevHadError || currHasError)
    {
        return ComputeAndCacheSemanticTokens(doc->uri, *doc->text);
    }

    const int currentVersion = GetDocumentStore().GetVersion(doc->uri);
    if (currentVersion >= 0 && snap.cachedVersion == currentVersion)
    {
        std::lock_guard<std::mutex> lock(m_semanticTokensMutex);
        const std::string newResultId = std::to_string(++m_semanticTokensRevision);
        if (auto it = m_semanticTokensCache.find(doc->uri); it != m_semanticTokensCache.end())
        {
            it->second.resultId = newResultId;
        }
        lsp::SemanticTokensDelta delta;
        delta.resultId = newResultId;
        delta.edits = {};
        LogInfo(fmt::format("[Semantic Tokens] Finished delta in {:.2f} ms for {} (unchanged)", timer.ElapsedMs(),
                            doc->uri));
        return delta;
    }

    lsp::SemanticTokens tokens = ComputeAndCacheSemanticTokens(doc->uri, *doc->text);
    lsp::SemanticTokensDelta delta;
    delta.resultId = tokens.resultId;
    delta.edits = features::ComputeSemanticTokensDelta(snap.previous, tokens.data);

    if (!AreSemanticTokensEditsValid(delta.edits))
    {
        return tokens;
    }
    LogInfo(fmt::format("[Semantic Tokens] Finished delta in {:.2f} ms for {} ({} edits)", timer.ElapsedMs(), doc->uri,
                        delta.edits.size()));
    return delta;
}

lsp::requests::TextDocument_SemanticTokens_Range::Result
Server::HandleRequestsTextDocument_SemanticTokens_Range(lsp::requests::TextDocument_SemanticTokens_Range::Params&& req)
{
    if (!m_config.features.enableSemanticTokens)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    const auto syntax =
        doc->docHandle && doc->docHandle->tree
            ? ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax
            : nullptr;
    const analysis::SemanticTokenFacts facts =
        syntax ? analysis::CollectSemanticTokenFacts(syntax->index, *doc->text) : analysis::SemanticTokenFacts{};
    features::SemanticTokensRequest sr{doc->uri, *doc->text, facts, m_symbolTable};
    sr.scopeRoot = m_scopeIndex.GetRoot(doc->uri);
    if (syntax && m_localScopeCollector && !sr.scopeRoot)
    {
        sr.scopeRoot = m_localScopeCollector->CollectScopesFromIndex(syntax->index, *doc->text);
    }
    sr.excludedLineRanges = ExcludedLineRanges(*doc->text);
    sr.range = codec::Decode(*doc->text, GetPositionEncoding(), req.range);

    lsp::SemanticTokens tokens = features::GetSemanticTokens(sr);
    codec::EncodeSemanticTokens(*doc->text, GetPositionEncoding(), tokens.data);
    return tokens;
}
} // namespace angel_lsp
