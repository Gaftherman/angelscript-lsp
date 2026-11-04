#include "analysis/index/NodeIndex.h"
#include "features/folding_range/FoldingRangeHandler.h"
#include "features/inlay_hint/InlayHintHandler.h"
#include "features/linked_editing/LinkedEditingRangeHandler.h"
#include "features/selection_range/SelectionRangeHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/Utils.h"

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

lsp::requests::TextDocument_LinkedEditingRange::Result
Server::HandleRequestsTextDocument_LinkedEditingRange(lsp::requests::TextDocument_LinkedEditingRange::Params&& req)
{
    if (!m_config.features.enableLinkedEditing)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    auto scopeRoot = m_scopeIndex.GetRoot(doc->uri);
    features::LinkedEditingRangeRequest lr{*doc->text, scopeRoot.get(),
                                           codec::Decode(*doc->text, GetPositionEncoding(), req.position)};

    auto ranges = features::GetLinkedEditingRanges(lr);
    if (!ranges.has_value())
    {
        return lsp::Null{};
    }
    for (auto& range : ranges->ranges)
    {
        codec::Encode(*doc->text, GetPositionEncoding(), range);
    }
    return ranges.value();
}

static lsp::requests::TextDocument_SelectionRange::Result
HandleRequestsTextDocument_SelectionRange(Server& server, lsp::requests::TextDocument_SelectionRange::Params&& req)
{
    if (!server.GetConfig().features.enableSelectionRange)
    {
        return lsp::Null{};
    }
    const auto doc = server.LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    std::vector<lsp::Position> positions;
    positions.reserve(req.positions.size());
    for (const auto& position : req.positions)
    {
        positions.push_back(codec::Decode(*doc->text, server.GetPositionEncoding(), position));
    }

    features::SelectionRangeRequest sr{*doc->text, doc->tree, positions};
    auto ranges = features::GetSelectionRanges(sr);
    if (ranges.empty())
    {
        return lsp::Null{};
    }

    for (auto& chain : ranges)
    {
        for (lsp::SelectionRange* link = &chain; link != nullptr; link = link->parent.get())
        {
            codec::Encode(*doc->text, server.GetPositionEncoding(), link->range);
        }
    }
    return ranges;
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

    const bool currHasError = doc->tree != nullptr && ts_node_has_error(ts_tree_root_node(doc->tree));
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
        doc->tree ? ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax
                  : nullptr;
    TSTree* tree = syntax ? syntax->tree.get() : nullptr;
    features::SemanticTokensRequest sr{doc->uri, *doc->text, tree, m_symbolTable};
    sr.scopeRoot = m_scopeIndex.GetRoot(doc->uri);
    sr.nodeIndex = syntax ? &syntax->index : nullptr;
    if (tree && m_localScopeCollector)
    {
        sr.scopeRoot = m_localScopeCollector->CollectScopesFromTree(syntax->index.Root(), *doc->text, sr.nodeIndex);
    }
    sr.excludedLineRanges = ExcludedLineRanges(*doc->text);
    sr.range = codec::Decode(*doc->text, GetPositionEncoding(), req.range);

    lsp::SemanticTokens tokens = features::GetSemanticTokens(sr);
    codec::EncodeSemanticTokens(*doc->text, GetPositionEncoding(), tokens.data);
    return tokens;
}

static lsp::requests::TextDocument_FoldingRange::Result
HandleRequestsTextDocument_FoldingRange(Server& server, lsp::requests::TextDocument_FoldingRange::Params&& req)
{
    if (!server.GetConfig().features.enableFoldingRange)
    {
        return lsp::Array<lsp::FoldingRange>{};
    }
    const auto doc = server.LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Array<lsp::FoldingRange>{};
    }

    utils::HighResTimer timer;
    features::FoldingRangeRequest fr{doc->uri, *doc->text, doc->tree};
    auto ranges = features::GetFoldingRanges(fr);
    const double elapsedMs = timer.ElapsedMs();
    if (ranges.has_value())
    {
        server.LogInfo(fmt::format("[Folding Ranges] Finished in {:.2f} ms for {} ({} ranges)", elapsedMs, doc->uri,
                                   ranges->size()));
        server.EncodeIn(*doc->text, ranges.value());
        return ranges.value();
    }
    server.LogInfo(fmt::format("[Folding Ranges] Finished in {:.2f} ms for {} (0 ranges)", elapsedMs, doc->uri));
    return lsp::Array<lsp::FoldingRange>{};
}

lsp::requests::TextDocument_InlayHint::Result
Server::HandleRequestsTextDocument_InlayHint(lsp::requests::TextDocument_InlayHint::Params&& req)
{
    if (!m_config.features.enableInlayHints)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    if (angel_lsp::utils::IsPredefinedFile(doc->uri, m_config.info.predefinedFileExtension))
    {
        return lsp::Array<lsp::InlayHint>{};
    }

    utils::HighResTimer timer;
    features::InlayHintRequest ihr{doc->uri,
                                   *doc->text,
                                   doc->tree,
                                   codec::Decode(*doc->text, GetPositionEncoding(), req.range),
                                   m_symbolTable,
                                   m_scopeIndex,
                                   m_config.features.inlayHintsSuppressWhenArgumentMatchesName,
                                   m_logger.get(),
                                   m_config.features.inlayHintsMaxParameters,
                                   m_config.features.inlayHintsMaxLength,
                                   m_config.features.inlayHintsOmittedDefaultArguments,
                                   &m_config,
                                   m_i18n.get()};
    auto hints = features::GetInlayHints(ihr);
    const double elapsedMs = timer.ElapsedMs();
    if (hints.has_value())
    {
        LogInfo(
            fmt::format("[Inlay Hints] Finished in {:.2f} ms for {} ({} hints)", elapsedMs, doc->uri, hints->size()));
        EncodeIn(*doc->text, hints.value());
        return hints.value();
    }
    return lsp::Null{};
}

void Server::RegisterTokensAndFormattingHandlers()
{
    m_messageHandler->add<lsp::requests::TextDocument_LinkedEditingRange>(
        [this](lsp::requests::TextDocument_LinkedEditingRange::Params&& req)
        { return HandleRequestsTextDocument_LinkedEditingRange(std::move(req)); });

    m_messageHandler->add<lsp::requests::TextDocument_SelectionRange>(
        [this](lsp::requests::TextDocument_SelectionRange::Params&& req)
        { return HandleRequestsTextDocument_SelectionRange(*this, std::move(req)); });

    m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Full>(
        [this](lsp::requests::TextDocument_SemanticTokens_Full::Params&& req)
        { return HandleRequestsTextDocument_SemanticTokens_Full(std::move(req)); });

    m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Full_Delta>(
        [this](lsp::requests::TextDocument_SemanticTokens_Full_Delta::Params&& req)
        { return HandleRequestsTextDocument_SemanticTokens_Full_Delta(std::move(req)); });

    m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Range>(
        [this](lsp::requests::TextDocument_SemanticTokens_Range::Params&& req)
        { return HandleRequestsTextDocument_SemanticTokens_Range(std::move(req)); });

    m_messageHandler->add<lsp::requests::TextDocument_FoldingRange>(
        [this](lsp::requests::TextDocument_FoldingRange::Params&& req)
        { return HandleRequestsTextDocument_FoldingRange(*this, std::move(req)); });

    m_messageHandler->add<lsp::requests::TextDocument_InlayHint>(
        [this](lsp::requests::TextDocument_InlayHint::Params&& req)
        { return HandleRequestsTextDocument_InlayHint(std::move(req)); });

    m_messageHandler->add<lsp::requests::InlayHint_Resolve>(
        [this](lsp::requests::InlayHint_Resolve::Params&& req)
        { return HandleRequestsInlayHint_Resolve(std::move(req)); });
}
} // namespace angel_lsp
