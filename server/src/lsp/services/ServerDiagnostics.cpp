#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/diagnostics/DiagnosticSuppression.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/PositionEncoding.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Utils.h"

#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
static lsp::DiagnosticSeverity ToProtocolSeverity(angel_lsp::analysis::DiagnosticSeverity severity)
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

/**
 * @brief Formats an LSP diagnostic message with an explicit severity and code prefix.
 * @param[in] severity AngelScript analysis severity.
 * @param[in] displayCode Formatted display code alias.
 * @param[in] rawMessage Original diagnostic message.
 * @return Prefixed message string.
 */
static std::string FormatPrefixedDiagnosticMessage(angel_lsp::analysis::DiagnosticSeverity severity,
                                                   std::string_view displayCode, std::string_view rawMessage)
{
    if (displayCode.empty() || rawMessage.starts_with("["))
    {
        return std::string(rawMessage);
    }

    std::string prefix;
    switch (severity)
    {
    case angel_lsp::analysis::DiagnosticSeverity::Error:
        prefix = fmt::format("[ERROR: {}] ", displayCode);
        break;
    case angel_lsp::analysis::DiagnosticSeverity::Warning:
        prefix = fmt::format("[WARN: {}] ", displayCode);
        break;
    case angel_lsp::analysis::DiagnosticSeverity::Hint:
        prefix = fmt::format("[HINT: {}] ", displayCode);
        break;
    default:
        prefix = fmt::format("[INFO: {}] ", displayCode);
        break;
    }
    return prefix + std::string(rawMessage);
}

/**
 * @brief Resolves a VS Code settings URI for a diagnostic code so the code renders as a clickable link.
 * @param[in] code Canonical or alias diagnostic code.
 * @return Settings deep-link URI string or empty string when no code is present.
 */

static std::string_view GetDiagnosticSettingsUri(std::string_view code)
{
    if (code.empty())
    {
        return {};
    }
    static constexpr CodeSettingPair kCodeUris[] = {
        {angel_lsp::diagnostics::codes::AccessorPortability,
         "vscode://settings/angelscript.diagnostics.reportAccessorPortability"},
        {"H402", "vscode://settings/angelscript.diagnostics.reportAccessorPortability"},
        {angel_lsp::diagnostics::codes::AccessorDisabled,
         "vscode://settings/angelscript.diagnostics.reportAccessorDisabled"},
        {"H403", "vscode://settings/angelscript.diagnostics.reportAccessorDisabled"},
        {angel_lsp::diagnostics::codes::BoolConversion,
         "vscode://settings/angelscript.diagnostics.reportBoolConversion"},
        {"H401", "vscode://settings/angelscript.diagnostics.reportBoolConversion"},
        {angel_lsp::diagnostics::codes::FuncdefMissing,
         "vscode://settings/angelscript.diagnostics.reportMissingFuncdef"},
        {"H404", "vscode://settings/angelscript.diagnostics.reportMissingFuncdef"},
        {angel_lsp::diagnostics::codes::IntegerDivision,
         "vscode://settings/angelscript.diagnostics.reportIntegerDivision"},
        {"H405", "vscode://settings/angelscript.diagnostics.reportIntegerDivision"},
        {angel_lsp::diagnostics::codes::PossibleNullDereference,
         "vscode://settings/angelscript.diagnostics.reportPossibleNullDereference"},
        {"W114", "vscode://settings/angelscript.diagnostics.reportPossibleNullDereference"},
        {angel_lsp::diagnostics::codes::HandleComparisonEquality,
         "vscode://settings/angelscript.diagnostics.reportHandleComparisonEquality"},
        {"W115", "vscode://settings/angelscript.diagnostics.reportHandleComparisonEquality"},
        {"W116", "vscode://settings/angelscript.diagnostics.missingAssetPathSeverity"},
        {angel_lsp::diagnostics::codes::UnusedFunction,
         "vscode://settings/angelscript.diagnostics.reportUnusedFunctions"},
        {"W107", "vscode://settings/angelscript.diagnostics.reportUnusedFunctions"},
        {angel_lsp::diagnostics::codes::UnusedMethod,
         "vscode://settings/angelscript.diagnostics.reportUnusedFunctions"},
        {"W108", "vscode://settings/angelscript.diagnostics.reportUnusedFunctions"},
        {angel_lsp::diagnostics::codes::UnusedGlobalVariable,
         "vscode://settings/angelscript.diagnostics.reportUnusedGlobalVariables"},
        {"W117", "vscode://settings/angelscript.diagnostics.reportUnusedGlobalVariables"},
        {angel_lsp::diagnostics::codes::UnusedClass, "vscode://settings/angelscript.diagnostics.reportUnusedClasses"},
        {"W109", "vscode://settings/angelscript.diagnostics.reportUnusedClasses"},
        {angel_lsp::diagnostics::codes::UnusedField, "vscode://settings/angelscript.diagnostics.reportUnusedMembers"},
        {"W104", "vscode://settings/angelscript.diagnostics.reportUnusedMembers"},
        {angel_lsp::diagnostics::codes::UnknownType, "vscode://settings/angelscript.diagnostics.reportUnknownTypes"},
        {"E002", "vscode://settings/angelscript.diagnostics.reportUnknownTypes"},
    };
    for (const auto& entry : kCodeUris)
    {
        if (entry.code == code)
        {
            return entry.uri;
        }
    }
    return "vscode://settings/angelscript.diagnosticSeverity";
}

void Server::EncodeIn(std::string_view text, lsp::Range& range) const
{
    codec::Encode(text, GetPositionEncoding(), range);
}

void Server::EncodeIn(std::string_view text, lsp::Hover& hover) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || !hover.range.has_value())
        return;

    codec::Encode(text, GetPositionEncoding(), hover.range.value());
}

static void EncodeSymbolsRecursive(const utils::LineIndex& lineIndex, std::string_view text,
                                   utils::PositionEncoding enc, std::vector<lsp::DocumentSymbol>& symbols)
{
    for (auto& symbol : symbols)
    {
        codec::Encode(lineIndex, text, enc, symbol.range);
        codec::Encode(lineIndex, text, enc, symbol.selectionRange);

        if (symbol.children.has_value())
        {
            EncodeSymbolsRecursive(lineIndex, text, enc, symbol.children.value());
        }
    }
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::TextEdit>& edits) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || edits.empty())
        return;

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& edit : edits)
        codec::Encode(lineIndex, text, GetPositionEncoding(), edit.range);
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentHighlight>& highlights) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || highlights.empty())
        return;

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& highlight : highlights)
        codec::Encode(lineIndex, text, GetPositionEncoding(), highlight.range);
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::FoldingRange>& ranges) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || ranges.empty())
        return;

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& range : ranges)
    {
        if (range.startCharacter.has_value())
        {
            const std::string_view line = lineIndex.Line(text, range.startLine);
            range.startCharacter =
                angel_lsp::utils::ByteToLspCharColumn(line, range.startCharacter.value(), GetPositionEncoding());
        }

        if (range.endCharacter.has_value())
        {
            const std::string_view line = lineIndex.Line(text, range.endLine);
            range.endCharacter =
                angel_lsp::utils::ByteToLspCharColumn(line, range.endCharacter.value(), GetPositionEncoding());
        }
    }
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentLink>& links) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || links.empty())
        return;

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& link : links)
        codec::Encode(lineIndex, text, GetPositionEncoding(), link.range);
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::InlayHint>& hints) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || hints.empty())
        return;

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& hint : hints)
    {
        codec::Encode(lineIndex, text, GetPositionEncoding(), hint.position);

        if (hint.textEdits.has_value())
        {
            for (auto& edit : hint.textEdits.value())
                codec::Encode(lineIndex, text, GetPositionEncoding(), edit.range);
        }
    }
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentSymbol>& symbols) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || symbols.empty())
        return;

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    EncodeSymbolsRecursive(lineIndex, text, GetPositionEncoding(), symbols);
}

void Server::EncodeIn(std::string_view text, std::vector<lsp::CodeLens>& lenses) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8 || lenses.empty())
        return;

    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);
    for (auto& lens : lenses)
        codec::Encode(lineIndex, text, GetPositionEncoding(), lens.range);
}

void Server::EncodeIn(std::string_view text, lsp::PrepareRenameResult& result) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
        return;

    if (auto* range = std::get_if<lsp::Range>(&result))
        codec::Encode(text, GetPositionEncoding(), *range);
    else if (auto* placeholder = std::get_if<lsp::PrepareRenamePlaceholder>(&result))
        codec::Encode(text, GetPositionEncoding(), placeholder->range);
}

void Server::EncodeAcrossDocuments(std::vector<lsp::Location>& locations) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
        return;

    for (auto& location : locations)
    {
        if (auto text = FindDocumentText(DocumentKey(location.uri.toString())))
            codec::Encode(*text, GetPositionEncoding(), location.range);
    }
}

void Server::EncodeAcrossDocuments(std::vector<lsp::SymbolInformation>& symbols) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
        return;

    for (auto& symbol : symbols)
    {
        if (auto text = FindDocumentText(DocumentKey(symbol.location.uri.toString())))
            codec::Encode(*text, GetPositionEncoding(), symbol.location.range);
    }
}

void Server::EncodeAcrossDocuments(lsp::WorkspaceEdit& edit) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
        return;

    if (edit.changes.has_value())
    {
        for (auto& [uri, edits] : edit.changes.value())
        {
            if (auto text = FindDocumentText(uri.toString()))
                EncodeIn(*text, edits);
        }
    }

    if (edit.documentChanges.has_value())
    {
        for (auto& docChange : edit.documentChanges.value())
        {
            if (std::holds_alternative<lsp::TextDocumentEdit>(docChange))
            {
                auto& docEdit = std::get<lsp::TextDocumentEdit>(docChange);
                if (auto text = FindDocumentText(DocumentKey(docEdit.textDocument.uri.toString())))
                {
                    for (auto& e : docEdit.edits)
                    {
                        if (std::holds_alternative<lsp::TextEdit>(e))
                        {
                            auto& te = std::get<lsp::TextEdit>(e);
                            codec::Encode(*text, GetPositionEncoding(), te.range);
                        }
                        else if (std::holds_alternative<lsp::AnnotatedTextEdit>(e))
                        {
                            auto& ate = std::get<lsp::AnnotatedTextEdit>(e);
                            codec::Encode(*text, GetPositionEncoding(), ate.range);
                        }
                        else if (std::holds_alternative<lsp::SnippetTextEdit>(e))
                        {
                            auto& ste = std::get<lsp::SnippetTextEdit>(e);
                            codec::Encode(*text, GetPositionEncoding(), ste.range);
                        }
                    }
                }
            }
        }
    }
}

void Server::EncodeRangesIn(const std::string& uri, std::vector<lsp::Range>& ranges) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
        return;

    if (auto text = FindDocumentText(uri))
    {
        for (auto& range : ranges)
            codec::Encode(*text, GetPositionEncoding(), range);
    }
}

void Server::EncodeAcrossDocuments(std::vector<lsp::CodeAction>& actions) const
{
    if (GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8)
        return;

    for (auto& action : actions)
    {
        if (action.edit.has_value())
            EncodeAcrossDocuments(action.edit.value());
    }
}

void Server::PublishDiagnostics(const std::string& uriStr,
                                const std::vector<angel_lsp::analysis::Diagnostic>& diagnostics, int version)
{
    auto docText = FindDocumentText(uriStr);
    if (version < 0)
    {
        version = GetDocumentVersion(uriStr);
    }
    PublishDiagnostics(PublishDiagnosticsRequest{
        .uriStr = uriStr,
        .text = docText ? *docText : std::string(),
        .diagnostics = diagnostics,
        .version = version,
        .generation = 0,
    });
}

std::vector<lsp::Diagnostic>
Server::ToProtocolDiagnostics(const std::string& text,
                              const std::vector<angel_lsp::analysis::Diagnostic>& diagnostics) const
{
    const auto excluded = ExcludedLineRanges(text);

    std::vector<lsp::Diagnostic> converted;
    converted.reserve(diagnostics.size());

    for (const auto& diag : diagnostics)
    {
        if (!excluded.empty() && angel_lsp::utils::IsLineExcluded(excluded, diag.range.start.line))
            continue;

        lsp::Diagnostic lspDiag;
        lspDiag.range.start.line = diag.range.start.line;
        lspDiag.range.start.character = diag.range.start.character;
        lspDiag.range.end.line = diag.range.end.line;
        lspDiag.range.end.character = diag.range.end.character;
        lspDiag.severity = ToProtocolSeverity(diag.severity);
        lspDiag.source = diag.source;
        std::string displayCode = angel_lsp::analysis::FormatDiagnosticDisplayCode(diag.code);
        lspDiag.code = diag.code;
        if (const std::string_view settingsUri = GetDiagnosticSettingsUri(diag.code); !settingsUri.empty())
        {
            lspDiag.codeDescription = lsp::CodeDescription{lsp::Uri::parse(settingsUri)};
        }
        lspDiag.message =
            FormatPrefixedDiagnosticMessage(diag.severity, displayCode.empty() ? diag.code : displayCode, diag.message);

        if (!diag.relatedInformation.empty())
        {
            std::vector<lsp::DiagnosticRelatedInformation> relInfos;
            relInfos.reserve(diag.relatedInformation.size());
            for (const auto& rel : diag.relatedInformation)
            {
                lsp::DiagnosticRelatedInformation lspRel;
                lspRel.message = rel.message;
                const std::string relUri = GetDocumentStore().GetClientUri(rel.fileUri);
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
            codec::Encode(text, m_sessionCoordinator->GetPositionEncoding(), lspDiag.range);

        converted.push_back(std::move(lspDiag));
    }

    return converted;
}

void Server::PublishInactiveRegions(const std::string& uriStr, const std::string& text)
{
    const auto rawTextOpt = GetDocumentStore().GetText(uriStr);
    const std::string& effectiveText = rawTextOpt ? *rawTextOpt : text;
    const auto excluded = ExcludedLineRanges(effectiveText);

    lsp::json::Array regions;
    for (const auto& range : excluded)
    {
        lsp::json::Object region;
        region["startLine"] = static_cast<lsp::json::Integer>(range.startLine);
        region["endLine"] = static_cast<lsp::json::Integer>(range.endLine);
        regions.push_back(lsp::json::Value(std::move(region)));
    }

    lsp::json::Object params;
    std::string clientUri = GetDocumentStore().GetClientUri(uriStr);
    params["uri"] = lsp::json::Value(std::move(clientUri));
    params["regions"] = std::move(regions);

    std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
    m_messageHandler->sendNotification("angelscript/inactiveRegions", lsp::json::Value(std::move(params)));
}

bool Server::IsPublishDiagnosticsStale(const PublishDiagnosticsRequest& request) const
{
    if (request.generation > 0 && !GetDocumentStore().IsCurrent(request.uriStr, request.generation, request.version))
    {
        return true;
    }

    if (request.version >= 0)
    {
        const int currentVersion = GetDocumentVersion(request.uriStr);
        if (currentVersion >= 0 && currentVersion != request.version)
        {
            return true;
        }
        if (currentVersion < 0 && !request.diagnostics.empty())
        {
            return true;
        }
    }
    return false;
}

void Server::CacheDiagnosticsSnapshot(const PublishDiagnosticsRequest& request,
                                      const std::vector<lsp::Diagnostic>& protocolDiagnostics)
{
    DiagnosticsSnapshot snapshot;
    snapshot.resultId = std::to_string(m_diagnosticsPipeline->NextRevision());
    snapshot.items = protocolDiagnostics;
    snapshot.textHash = std::hash<std::string>{}(request.text);
    snapshot.version = request.version;
    snapshot.generation = request.generation;
    snapshot.configRevision = m_sessionCoordinator->GetConfigRevision();
    m_diagnosticsPipeline->CacheDiagnosticsSnapshot(request.uriStr, std::move(snapshot));
}

void Server::NotifyClientDiagnostics(lsp::notifications::TextDocument_PublishDiagnostics::Params params)
{
    if (m_sessionCoordinator->GetClientPullsDiagnostics())
    {
        const bool isBatchQueued =
            m_diagnosticsPipeline->HasQueuedWork() && !GetDocumentStore().IsOpen(DocumentKey(params.uri.toString()));
        if (m_sessionCoordinator->GetClientSupportsDiagnosticRefresh() && !isBatchQueued)
        {
            std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
            if (m_messageHandler)
            {
                m_messageHandler->sendRequest(
                    "workspace/diagnostic/refresh", std::nullopt, [](lsp::json::Value&&) {},
                    [](const lsp::ResponseError&) {});
            }
        }
        if (params.diagnostics.empty())
        {
            std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
            if (m_messageHandler)
            {
                m_messageHandler->sendNotification<lsp::notifications::TextDocument_PublishDiagnostics>(
                    std::move(params));
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

void Server::PublishDiagnostics(const PublishDiagnosticsRequest& request)
{
    if (IsPublishDiagnosticsStale(request))
    {
        return;
    }

    lsp::notifications::TextDocument_PublishDiagnostics::Params params;
    if (request.version >= 0)
    {
        params.version = request.version;
    }

    const std::string outgoingUri = GetDocumentStore().GetClientUri(request.uriStr);
    params.uri = lsp::DocumentUri(lsp::Uri::parse(outgoingUri));

    const auto rawTextOpt = GetDocumentStore().GetText(request.uriStr);
    const std::string& effectiveText = rawTextOpt ? *rawTextOpt : request.text;

    params.diagnostics = ToProtocolDiagnostics(effectiveText, request.diagnostics);

    if (GetDocumentStore().IsOpen(request.uriStr))
    {
        PublishInactiveRegions(request.uriStr, effectiveText);
    }

    CacheDiagnosticsSnapshot(request, params.diagnostics);

    NotifyClientDiagnostics(std::move(params));
}

void Server::PublishDiagnostics(const std::string& uriStr, const std::string& text,
                                const std::vector<angel_lsp::analysis::Diagnostic>& diagnostics, int version)
{
    PublishDiagnostics(PublishDiagnosticsRequest{
        .uriStr = uriStr,
        .text = text,
        .diagnostics = diagnostics,
        .version = version,
        .generation = GetDocumentStore().GetGeneration(uriStr),
    });
}
} // namespace angel_lsp
