#include "analysis/index/NodeIndex.h"
#include "features/folding_range/FoldingRangeHandler.h"
#include "features/inlay_hint/InlayHintHandler.h"
#include "features/linked_editing/LinkedEditingRangeHandler.h"
#include "features/selection_range/SelectionRangeHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/Utils.h"

#include <spdlog/fmt/fmt.h>
#include <utility>
#include <vector>

namespace angel_lsp
{
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

lsp::requests::TextDocument_SelectionRange::Result
Server::HandleRequestsTextDocument_SelectionRange(lsp::requests::TextDocument_SelectionRange::Params&& req)
{
    if (!m_config.features.enableSelectionRange)
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
    const auto* nodeIndex = syntax ? &syntax->index : nullptr;

    std::vector<lsp::Position> decodedPositions;
    decodedPositions.reserve(req.positions.size());
    for (const auto& pos : req.positions)
    {
        decodedPositions.push_back(codec::Decode(*doc->text, GetPositionEncoding(), pos));
    }

    features::SelectionRangeRequest sr{*doc->text, nodeIndex, decodedPositions};
    auto ranges = features::GetSelectionRanges(sr);
    if (ranges.empty())
    {
        return lsp::Null{};
    }

    for (auto& chain : ranges)
    {
        for (lsp::SelectionRange* link = &chain; link != nullptr; link = link->parent.get())
        {
            codec::Encode(*doc->text, GetPositionEncoding(), link->range);
        }
    }
    return ranges;
}

lsp::requests::TextDocument_FoldingRange::Result
Server::HandleRequestsTextDocument_FoldingRange(lsp::requests::TextDocument_FoldingRange::Params&& req)
{
    if (!m_config.features.enableFoldingRange)
    {
        return lsp::Array<lsp::FoldingRange>{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Array<lsp::FoldingRange>{};
    }

    const auto syntax =
        doc->docHandle && doc->docHandle->tree
            ? ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax
            : nullptr;
    const auto* nodeIndex = syntax ? &syntax->index : nullptr;

    utils::HighResTimer timer;
    features::FoldingRangeRequest fr{doc->uri, *doc->text, nodeIndex};
    auto ranges = features::GetFoldingRanges(fr);
    const double elapsedMs = timer.ElapsedMs();
    if (ranges.has_value())
    {
        LogInfo(fmt::format("[Folding Ranges] Finished in {:.2f} ms for {} ({} ranges)", elapsedMs, doc->uri,
                            ranges->size()));
        EncodeIn(*doc->text, ranges.value());
        return ranges.value();
    }
    LogInfo(fmt::format("[Folding Ranges] Finished in {:.2f} ms for {} (0 ranges)", elapsedMs, doc->uri));
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
    const auto syntax =
        doc->docHandle && doc->docHandle->tree
            ? ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax
            : nullptr;
    const auto* nodeIndex = syntax ? &syntax->index : nullptr;
    features::InlayHintRequest ihr{doc->uri,
                                   *doc->text,
                                   nodeIndex,
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
        { return HandleRequestsTextDocument_SelectionRange(std::move(req)); });

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
        { return HandleRequestsTextDocument_FoldingRange(std::move(req)); });

    m_messageHandler->add<lsp::requests::TextDocument_InlayHint>(
        [this](lsp::requests::TextDocument_InlayHint::Params&& req)
        { return HandleRequestsTextDocument_InlayHint(std::move(req)); });

    m_messageHandler->add<lsp::requests::InlayHint_Resolve>(
        [this](lsp::requests::InlayHint_Resolve::Params&& req)
        { return HandleRequestsInlayHint_Resolve(std::move(req)); });
}
} // namespace angel_lsp
