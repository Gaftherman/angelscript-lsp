#include "lsp/Server.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "features/linked_editing/LinkedEditingRangeHandler.h"
#include "features/selection_range/SelectionRangeHandler.h"
#include "features/folding_range/FoldingRangeHandler.h"
#include "features/inlay_hint/InlayHintHandler.h"
#include "lsp/PositionCodec.h"

namespace angel_lsp
{
    void Server::RegisterTokensAndFormattingHandlers()
    {
        m_messageHandler->add<lsp::requests::TextDocument_LinkedEditingRange>(
            [this](lsp::requests::TextDocument_LinkedEditingRange::Params &&req) -> lsp::requests::TextDocument_LinkedEditingRange::Result
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
                features::LinkedEditingRangeRequest lr{
                    *doc->text, scopeRoot.get(),
                    codec::Decode(*doc->text, m_positionEncoding, req.position)
                };

                auto ranges = features::GetLinkedEditingRanges(lr);
                if (!ranges.has_value())
                {
                    return lsp::Null{};
                }
                for (auto &range : ranges->ranges)
                {
                    codec::Encode(*doc->text, m_positionEncoding, range);
                }
                return ranges.value();
            });

        m_messageHandler->add<lsp::requests::TextDocument_SelectionRange>(
            [this](lsp::requests::TextDocument_SelectionRange::Params &&req) -> lsp::requests::TextDocument_SelectionRange::Result
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

                std::vector<lsp::Position> positions;
                positions.reserve(req.positions.size());
                for (const auto &position : req.positions)
                {
                    positions.push_back(codec::Decode(*doc->text, m_positionEncoding, position));
                }

                features::SelectionRangeRequest sr{ *doc->text, doc->tree, positions };
                auto ranges = features::GetSelectionRanges(sr);
                if (ranges.empty())
                {
                    return lsp::Null{};
                }

                for (auto &chain : ranges)
                {
                    for (lsp::SelectionRange *link = &chain; link != nullptr; link = link->parent.get())
                    {
                        codec::Encode(*doc->text, m_positionEncoding, link->range);
                    }
                }
                return ranges;
            });

        m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Full>(
            [this](lsp::requests::TextDocument_SemanticTokens_Full::Params &&req) -> lsp::requests::TextDocument_SemanticTokens_Full::Result
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

                return ComputeAndCacheSemanticTokens(doc->uri, *doc->text);
            });

        m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Full_Delta>(
            [this](lsp::requests::TextDocument_SemanticTokens_Full_Delta::Params &&req) -> lsp::requests::TextDocument_SemanticTokens_Full_Delta::Result
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

                const auto cached = m_semanticTokensCache.find(doc->uri);
                const bool canDiff = cached != m_semanticTokensCache.end() &&
                                     cached->second.resultId == req.previousResultId;

                if (!canDiff)
                {
                    return ComputeAndCacheSemanticTokens(doc->uri, *doc->text);
                }

                const bool prevHadError = cached->second.hasError;
                const bool currHasError = doc->tree != nullptr && ts_node_has_error(ts_tree_root_node(doc->tree));

                if (prevHadError || currHasError)
                {
                    return ComputeAndCacheSemanticTokens(doc->uri, *doc->text);
                }

                int currentVersion = -1;
                if (auto it = m_documentVersions.find(doc->uri); it != m_documentVersions.end())
                {
                    currentVersion = it->second;
                }

                if (currentVersion >= 0 && cached->second.version >= 0 &&
                    cached->second.version == currentVersion && !currHasError)
                {
                    const std::string newResultId = std::to_string(++m_semanticTokensRevision);
                    cached->second.resultId = newResultId;
                    lsp::SemanticTokensDelta delta;
                    delta.resultId = newResultId;
                    delta.edits = {};
                    return delta;
                }

                const std::vector<lsp::uint> previous = std::move(cached->second.data);
                lsp::SemanticTokens tokens = ComputeAndCacheSemanticTokens(doc->uri, *doc->text);

                lsp::SemanticTokensDelta delta;
                delta.resultId = tokens.resultId;
                delta.edits = features::ComputeSemanticTokensDelta(previous, tokens.data);

                for (const auto &edit : delta.edits)
                {
                    if (edit.start % 5 != 0 || edit.deleteCount % 5 != 0 ||
                        (edit.data.has_value() && edit.data->size() % 5 != 0))
                    {
                        return tokens;
                    }
                }

                return delta;
            });

        m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Range>(
            [this](lsp::requests::TextDocument_SemanticTokens_Range::Params &&req) -> lsp::requests::TextDocument_SemanticTokens_Range::Result
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

                features::SemanticTokensRequest sr{ doc->uri, *doc->text, doc->tree, m_symbolTable, m_scopeIndex.GetRoot(doc->uri) };
                sr.excludedLineRanges = ExcludedLineRanges(*doc->text);
                sr.range = codec::Decode(*doc->text, m_positionEncoding, req.range);

                lsp::SemanticTokens tokens = features::GetSemanticTokens(sr);
                codec::EncodeSemanticTokens(*doc->text, m_positionEncoding, tokens.data);
                return tokens;
            });

        m_messageHandler->add<lsp::requests::TextDocument_FoldingRange>(
            [this](lsp::requests::TextDocument_FoldingRange::Params &&req) -> lsp::requests::TextDocument_FoldingRange::Result
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

                features::FoldingRangeRequest fr{ doc->uri, *doc->text, doc->tree };
                auto ranges = features::GetFoldingRanges(fr);
                if (ranges.has_value())
                {
                    EncodeIn(*doc->text, ranges.value());
                    return ranges.value();
                }
                return lsp::Array<lsp::FoldingRange>{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_InlayHint>(
            [this](lsp::requests::TextDocument_InlayHint::Params &&req) -> lsp::requests::TextDocument_InlayHint::Result
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

                features::InlayHintRequest ihr{
                    doc->uri,
                    *doc->text,
                    doc->tree,
                    codec::Decode(*doc->text, m_positionEncoding, req.range),
                    m_symbolTable,
                    m_scopeIndex,
                    m_config.features.inlayHintsSuppressWhenArgumentMatchesName,
                    m_logger.get()
                };
                auto hints = features::GetInlayHints(ihr);
                if (hints.has_value())
                {
                    EncodeIn(*doc->text, hints.value());
                    return hints.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::InlayHint_Resolve>(
            [this](lsp::requests::InlayHint_Resolve::Params &&req) -> lsp::requests::InlayHint_Resolve::Result
            {
                return this->HandleRequestsInlayHint_Resolve(std::move(req));
            });
    }
}
