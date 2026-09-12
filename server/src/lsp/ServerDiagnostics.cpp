#include "lsp/Server.h"
#include "lsp/PositionCodec.h"
#include "utils/Utils.h"
#include "utils/PositionEncoding.h"
#include "utils/PreprocessorRegions.h"
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
    namespace
    {
        lsp::DiagnosticSeverity ToProtocolSeverity(angel_lsp::analysis::DiagnosticSeverity severity)
        {
            switch (severity)
            {
            case angel_lsp::analysis::DiagnosticSeverity::Error:
                return lsp::DiagnosticSeverity::Error;
            case angel_lsp::analysis::DiagnosticSeverity::Warning:
                return lsp::DiagnosticSeverity::Warning;
            case angel_lsp::analysis::DiagnosticSeverity::Information:
                return lsp::DiagnosticSeverity::Information;
            case angel_lsp::analysis::DiagnosticSeverity::Hint:
                return lsp::DiagnosticSeverity::Hint;
            }
            return lsp::DiagnosticSeverity::Error;
        }
    }

    void Server::EncodeIn(std::string_view text, lsp::Range &range) const
    {
        codec::Encode(text, m_positionEncoding, range);
    }

    void Server::EncodeIn(std::string_view text, lsp::Hover &hover) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8 || !hover.range.has_value())
            return;

        codec::Encode(text, m_positionEncoding, hover.range.value());
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::TextEdit> &edits) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &edit : edits)
            codec::Encode(text, m_positionEncoding, edit.range);
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentHighlight> &highlights) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &highlight : highlights)
            codec::Encode(text, m_positionEncoding, highlight.range);
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::FoldingRange> &ranges) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &range : ranges)
        {
            if (range.startCharacter.has_value())
            {
                range.startCharacter = angel_lsp::utils::ByteToLspCharColumn(
                    angel_lsp::utils::GetLine(text, range.startLine), range.startCharacter.value(), m_positionEncoding);
            }

            if (range.endCharacter.has_value())
            {
                range.endCharacter = angel_lsp::utils::ByteToLspCharColumn(
                    angel_lsp::utils::GetLine(text, range.endLine), range.endCharacter.value(), m_positionEncoding);
            }
        }
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentLink> &links) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &link : links)
            codec::Encode(text, m_positionEncoding, link.range);
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::InlayHint> &hints) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &hint : hints)
        {
            codec::Encode(text, m_positionEncoding, hint.position);

            if (hint.textEdits.has_value())
            {
                for (auto &edit : hint.textEdits.value())
                    codec::Encode(text, m_positionEncoding, edit.range);
            }
        }
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentSymbol> &symbols) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &symbol : symbols)
        {
            codec::Encode(text, m_positionEncoding, symbol.range);
            codec::Encode(text, m_positionEncoding, symbol.selectionRange);

            if (symbol.children.has_value())
                EncodeIn(text, symbol.children.value());
        }
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::CodeLens> &lenses) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &lens : lenses)
            codec::Encode(text, m_positionEncoding, lens.range);
    }

    void Server::EncodeIn(std::string_view text, lsp::PrepareRenameResult &result) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        if (auto *range = std::get_if<lsp::Range>(&result))
            codec::Encode(text, m_positionEncoding, *range);
        else if (auto *placeholder = std::get_if<lsp::PrepareRenamePlaceholder>(&result))
            codec::Encode(text, m_positionEncoding, placeholder->range);
    }

    void Server::EncodeAcrossDocuments(std::vector<lsp::Location> &locations) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &location : locations)
        {
            if (const std::string *text = FindDocumentText(DocumentKey(location.uri.toString())))
                codec::Encode(*text, m_positionEncoding, location.range);
        }
    }

    void Server::EncodeAcrossDocuments(std::vector<lsp::SymbolInformation> &symbols) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &symbol : symbols)
        {
            if (const std::string *text = FindDocumentText(DocumentKey(symbol.location.uri.toString())))
                codec::Encode(*text, m_positionEncoding, symbol.location.range);
        }
    }

    void Server::EncodeAcrossDocuments(lsp::WorkspaceEdit &edit) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        if (edit.changes.has_value())
        {
            for (auto &[uri, edits] : edit.changes.value())
            {
                if (const std::string *text = FindDocumentText(uri.toString()))
                    EncodeIn(*text, edits);
            }
        }

        if (edit.documentChanges.has_value())
        {
            for (auto &docChange : edit.documentChanges.value())
            {
                if (std::holds_alternative<lsp::TextDocumentEdit>(docChange))
                {
                    auto &docEdit = std::get<lsp::TextDocumentEdit>(docChange);
                    if (const std::string *text = FindDocumentText(DocumentKey(docEdit.textDocument.uri.toString())))
                    {
                        for (auto &e : docEdit.edits)
                        {
                            if (std::holds_alternative<lsp::TextEdit>(e))
                            {
                                auto &te = std::get<lsp::TextEdit>(e);
                                codec::Encode(*text, m_positionEncoding, te.range);
                            }
                            else if (std::holds_alternative<lsp::AnnotatedTextEdit>(e))
                            {
                                auto &ate = std::get<lsp::AnnotatedTextEdit>(e);
                                codec::Encode(*text, m_positionEncoding, ate.range);
                            }
                            else if (std::holds_alternative<lsp::SnippetTextEdit>(e))
                            {
                                auto &ste = std::get<lsp::SnippetTextEdit>(e);
                                codec::Encode(*text, m_positionEncoding, ste.range);
                            }
                        }
                    }
                }
            }
        }
    }

    void Server::EncodeRangesIn(const std::string &uri, std::vector<lsp::Range> &ranges) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        if (const std::string *text = FindDocumentText(uri))
        {
            for (auto &range : ranges)
                codec::Encode(*text, m_positionEncoding, range);
        }
    }

    void Server::EncodeAcrossDocuments(std::vector<lsp::CodeAction> &actions) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &action : actions)
        {
            if (action.edit.has_value())
                EncodeAcrossDocuments(action.edit.value());
        }
    }

    void Server::PublishDiagnostics(const std::string &uriStr, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics, int version)
    {
        const std::string *docText = FindDocumentText(uriStr);
        if (version < 0)
        {
            version = GetDocumentVersion(uriStr);
        }
        PublishDiagnostics(uriStr, docText ? *docText : std::string(), diagnostics, version);
    }

    std::vector<lsp::Diagnostic> Server::ToProtocolDiagnostics(const std::string &text, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics) const
    {
        const auto excluded = ExcludedLineRanges(text);

        std::vector<lsp::Diagnostic> converted;
        converted.reserve(diagnostics.size());

        for (const auto &diag : diagnostics)
        {
            if (!excluded.empty() && angel_lsp::utils::IsLineExcluded(excluded, diag.range.start.line))
                continue;

            lsp::Diagnostic lspDiag;
            lspDiag.range.start.line = diag.range.start.line;
            lspDiag.range.start.character = diag.range.start.character;
            lspDiag.range.end.line = diag.range.end.line;
            lspDiag.range.end.character = diag.range.end.character;
            lspDiag.message = diag.message;
            lspDiag.severity = ToProtocolSeverity(diag.severity);
            lspDiag.source = diag.source;
            lspDiag.code = diag.code;

            if (!diag.relatedInformation.empty())
            {
                std::vector<lsp::DiagnosticRelatedInformation> relInfos;
                relInfos.reserve(diag.relatedInformation.size());
                for (const auto &rel : diag.relatedInformation)
                {
                    lsp::DiagnosticRelatedInformation lspRel;
                    lspRel.message = rel.message;
                    const auto clientRelUri = m_clientUriByKey.find(rel.fileUri);
                    const std::string &relUri = clientRelUri != m_clientUriByKey.end() ? clientRelUri->second : rel.fileUri;
                    lspRel.location.uri = lsp::DocumentUri(lsp::Uri::parse(relUri));
                    lspRel.location.range.start.line = rel.range.start.line;
                    lspRel.location.range.start.character = rel.range.start.character;
                    lspRel.location.range.end.line = rel.range.end.line;
                    lspRel.location.range.end.character = rel.range.end.character;
                    relInfos.push_back(std::move(lspRel));
                }
                lspDiag.relatedInformation = std::move(relInfos);
            }

            if (!text.empty())
                codec::Encode(text, m_positionEncoding, lspDiag.range);

            converted.push_back(std::move(lspDiag));
        }

        return converted;
    }

    void Server::PublishInactiveRegions(const std::string &uriStr, const std::string &text)
    {
        const auto excluded = ExcludedLineRanges(text);

        lsp::json::Array regions;
        for (const auto &range : excluded)
        {
            lsp::json::Object region;
            region["startLine"] = static_cast<lsp::json::Integer>(range.startLine);
            region["endLine"] = static_cast<lsp::json::Integer>(range.endLine);
            regions.push_back(lsp::json::Value(std::move(region)));
        }

        lsp::json::Object params;

        const auto clientUri = m_clientUriByKey.find(uriStr);
        params["uri"] = lsp::json::Value(
            std::string(clientUri != m_clientUriByKey.end() ? clientUri->second : uriStr));
        params["regions"] = std::move(regions);

        std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
        m_messageHandler->sendNotification("angelscript/inactiveRegions",
                                           lsp::json::Value(std::move(params)));
    }

    void Server::PublishDiagnostics(const std::string &uriStr, const std::string &text, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics, int version)
    {
        if (version >= 0)
        {
            const int currentVersion = GetDocumentVersion(uriStr);
            if (currentVersion >= 0 && currentVersion != version)
            {
                return;
            }
            if (currentVersion < 0 && !diagnostics.empty())
            {
                return;
            }
        }

        lsp::notifications::TextDocument_PublishDiagnostics::Params params;
        if (version >= 0)
        {
            params.version = version;
        }

        const auto clientUri = m_clientUriByKey.find(uriStr);
        const std::string &outgoingUri = clientUri != m_clientUriByKey.end() ? clientUri->second : uriStr;
        params.uri = lsp::DocumentUri(lsp::Uri::parse(outgoingUri));

        params.diagnostics = ToProtocolDiagnostics(text, diagnostics);

        PublishInactiveRegions(uriStr, text);

        {
            std::lock_guard<std::mutex> cacheLock(m_diagnosticsCacheMutex);
            DiagnosticsSnapshot snapshot;
            snapshot.resultId = std::to_string(++m_diagnosticsRevision);
            snapshot.items = params.diagnostics;
            snapshot.textHash = std::hash<std::string>{}(text);
            snapshot.version = version;
            m_diagnosticsCache[uriStr] = std::move(snapshot);
        }

        if (m_clientPullsDiagnostics)
        {
            if (m_clientSupportsDiagnosticRefresh)
            {
                std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
                if (m_messageHandler)
                {
                    m_messageHandler->sendRequest(
                        "workspace/diagnostic/refresh",
                        std::nullopt,
                        [](lsp::json::Value &&) {},
                        [](const lsp::ResponseError &) {});
                }
            }
            return;
        }

        std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
        if (m_messageHandler)
        {
            m_messageHandler->sendNotification<lsp::notifications::TextDocument_PublishDiagnostics>(std::move(params));
        }
    }
}
