#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/diagnostics/DiagnosticSuppression.h"
#include "lsp/Server.h"
#include "lsp/services/ServerDiagnosticFormatting.h"
#include "lsp/state/PositionCodec.h"
#include "utils/PositionEncoding.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Utils.h"

#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
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
Server::ToProtocolDiagnostics(const std::string& text, const std::vector<angel_lsp::analysis::Diagnostic>& diagnostics,
                              const std::vector<utils::ExcludedLineRange>& excluded) const
{
    std::vector<lsp::Diagnostic> converted;
    converted.reserve(diagnostics.size());

    for (const auto& diag : diagnostics)
    {
        if (!excluded.empty() && angel_lsp::utils::IsLineExcluded(excluded, diag.range.start.line))
        {
            continue;
        }

        lsp::Diagnostic lspDiag;
        lspDiag.range.start.line = diag.range.start.line;
        lspDiag.range.start.character = diag.range.start.character;
        lspDiag.range.end.line = diag.range.end.line;
        lspDiag.range.end.character = diag.range.end.character;
        lspDiag.severity = ToProtocolSeverity(diag.severity);
        lspDiag.source = std::string(diag.source);
        const std::string_view displayCode = angel_lsp::analysis::FormatDiagnosticDisplayCode(diag.code);
        lspDiag.code = std::string(diag.code);
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
        {
            codec::Encode(text, m_sessionCoordinator->GetPositionEncoding(), lspDiag.range);
        }

        converted.push_back(std::move(lspDiag));
    }

    return converted;
}

void Server::PublishInactiveRegions(const std::string& uriStr, const std::vector<utils::ExcludedLineRange>& excluded)
{
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
    snapshot.closedRevision = request.closedRevision;
    m_diagnosticsPipeline->CacheDiagnosticsSnapshot(request.uriStr, std::move(snapshot));
}

void Server::NotifyClientDiagnostics(lsp::notifications::TextDocument_PublishDiagnostics::Params params)
{
    if (m_sessionCoordinator->GetClientPullsDiagnostics())
    {
        const std::string docKey = DocumentKey(params.uri.toString());
        const bool isDocOpen = GetDocumentStore().IsOpen(docKey);
        const bool isBatchQueued = m_diagnosticsPipeline->HasQueuedWork() && !isDocOpen;
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

    const auto rawText = GetDocumentStore().GetTextShared(request.uriStr);
    const std::string& effectiveText = rawText ? *rawText : request.text;

    const auto fallback =
        request.preprocessing ? std::vector<utils::ExcludedLineRange>{} : ExcludedLineRanges(effectiveText);
    const auto& excluded = request.preprocessing ? request.preprocessing->excluded : fallback;
    params.diagnostics = ToProtocolDiagnostics(effectiveText, request.diagnostics, excluded);

    if (GetDocumentStore().IsOpen(request.uriStr))
    {
        PublishInactiveRegions(request.uriStr, excluded);
    }

    CacheDiagnosticsSnapshot(request, params.diagnostics);

    NotifyClientDiagnostics(std::move(params));
}

void Server::PublishDiagnostics(const std::string& uriStr, const std::string& text,
                                const std::vector<angel_lsp::analysis::Diagnostic>& diagnostics, int version,
                                std::shared_ptr<const utils::PreprocessorScan> preprocessing)
{
    PublishDiagnostics(PublishDiagnosticsRequest{
        .uriStr = uriStr,
        .text = text,
        .diagnostics = diagnostics,
        .version = version,
        .generation = GetDocumentStore().GetGeneration(uriStr),
        .preprocessing = std::move(preprocessing),
    });
}
} // namespace angel_lsp
