#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/Utils.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{

static void RegisterNavigationHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::TextDocument_Hover>([&server](lsp::requests::TextDocument_Hover::Params&& req)
                                                   { return server.HandleRequestsTextDocument_Hover(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Definition>(
        [&server](lsp::requests::TextDocument_Definition::Params&& req)
        { return server.HandleRequestsTextDocument_Definition(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Moniker>(
        [&server](lsp::requests::TextDocument_Moniker::Params&& req)
        { return server.HandleRequestsTextDocument_Moniker(std::move(req)); });

    handler.add<lsp::requests::TextDocument_InlineCompletion>(
        [](lsp::requests::TextDocument_InlineCompletion::Params&&)
            -> lsp::requests::TextDocument_InlineCompletion::Result { return lsp::Null{}; });

    handler.add<lsp::requests::TextDocument_Declaration>(
        [&server](lsp::requests::TextDocument_Declaration::Params&& req)
        { return server.HandleRequestsTextDocument_Declaration(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Implementation>(
        [&server](lsp::requests::TextDocument_Implementation::Params&& req)
        { return server.HandleRequestsTextDocument_Implementation(std::move(req)); });

    handler.add<lsp::requests::TextDocument_TypeDefinition>(
        [&server](lsp::requests::TextDocument_TypeDefinition::Params&& req)
        { return server.HandleRequestsTextDocument_TypeDefinition(std::move(req)); });

    handler.add<lsp::requests::TextDocument_References>(
        [&server](lsp::requests::TextDocument_References::Params&& req)
        { return server.HandleRequestsTextDocument_References(std::move(req)); });

    handler.add<lsp::requests::TextDocument_DocumentHighlight>(
        [&server](lsp::requests::TextDocument_DocumentHighlight::Params&& req)
        { return server.HandleRequestsTextDocument_DocumentHighlight(std::move(req)); });

    handler.add("textDocument/breakpointLocations", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsTextDocument_BreakpointLocations(std::move(params)); });

    handler.add("angelscript/breakpointLocations", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsTextDocument_BreakpointLocations(std::move(params)); });

    handler.add("textDocument/evaluatableExpression", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsTextDocument_EvaluatableExpression(std::move(params)); });

    handler.add("angelscript/evaluatableExpression", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsTextDocument_EvaluatableExpression(std::move(params)); });

    handler.add("angelscript/debugCompletion", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsAngelscript_DebugCompletion(std::move(params)); });
}

static void RegisterEditingHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::TextDocument_Completion>(
        [&server](lsp::requests::TextDocument_Completion::Params&& req)
        { return server.HandleRequestsTextDocument_Completion(std::move(req)); });

    handler.add<lsp::requests::CompletionItem_Resolve>(
        [&server](lsp::requests::CompletionItem_Resolve::Params&& req)
        { return server.HandleRequestsCompletionItem_Resolve(std::move(req)); });

    handler.add<lsp::requests::TextDocument_SignatureHelp>(
        [&server](lsp::requests::TextDocument_SignatureHelp::Params&& req)
        { return server.HandleRequestsTextDocument_SignatureHelp(std::move(req)); });

    handler.add<lsp::requests::TextDocument_PrepareRename>(
        [&server](lsp::requests::TextDocument_PrepareRename::Params&& req)
        { return server.HandleRequestsTextDocument_PrepareRename(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Rename>(
        [&server](lsp::requests::TextDocument_Rename::Params&& req)
        { return server.HandleRequestsTextDocument_Rename(std::move(req)); });

    handler.add<lsp::requests::TextDocument_CodeAction>(
        [&server](lsp::requests::TextDocument_CodeAction::Params&& req)
        { return server.HandleRequestsTextDocument_CodeAction(std::move(req)); });

    handler.add<lsp::requests::CodeAction_Resolve>([&server](lsp::requests::CodeAction_Resolve::Params&& req)
                                                   { return server.HandleRequestsCodeAction_Resolve(std::move(req)); });
}

static void RegisterFormattingAndSymbolHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::TextDocument_Diagnostic>(
        [&server](lsp::requests::TextDocument_Diagnostic::Params&& params)
        { return server.HandleRequestsTextDocument_Diagnostic(std::move(params)); });

    handler.add<lsp::requests::TextDocument_DocumentSymbol>(
        [&server](lsp::requests::TextDocument_DocumentSymbol::Params&& req)
        { return server.HandleRequestsTextDocument_DocumentSymbol(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Formatting>(
        [&server](lsp::requests::TextDocument_Formatting::Params&& req)
        { return server.HandleRequestsTextDocument_Formatting(std::move(req)); });

    handler.add<lsp::requests::TextDocument_RangeFormatting>(
        [&server](lsp::requests::TextDocument_RangeFormatting::Params&& req)
        { return server.HandleRequestsTextDocument_RangeFormatting(std::move(req)); });

    handler.add<lsp::requests::TextDocument_RangesFormatting>(
        [&server](lsp::requests::TextDocument_RangesFormatting::Params&& req)
        { return server.HandleRequestsTextDocument_RangesFormatting(std::move(req)); });

    handler.add<lsp::requests::TextDocument_OnTypeFormatting>(
        [&server](lsp::requests::TextDocument_OnTypeFormatting::Params&& req)
        { return server.HandleRequestsTextDocument_OnTypeFormatting(std::move(req)); });

    handler.add<lsp::requests::TextDocument_DocumentLink>(
        [&server](lsp::requests::TextDocument_DocumentLink::Params&& req)
        { return server.HandleRequestsTextDocument_DocumentLink(std::move(req)); });

    handler.add<lsp::requests::DocumentLink_Resolve>(
        [&server](lsp::requests::DocumentLink_Resolve::Params&& req)
        { return server.HandleRequestsDocumentLink_Resolve(std::move(req)); });

    handler.add<lsp::requests::TextDocument_CodeLens>(
        [&server](lsp::requests::TextDocument_CodeLens::Params&& req)
        { return server.HandleRequestsTextDocument_CodeLens(std::move(req)); });

    handler.add<lsp::requests::CodeLens_Resolve>([&server](lsp::requests::CodeLens_Resolve::Params&& req)
                                                 { return server.HandleRequestsCodeLens_Resolve(std::move(req)); });
}

void Server::RegisterTextDocumentHandlers()
{
    RegisterNavigationHandlers(*this, *m_messageHandler);
    RegisterEditingHandlers(*this, *m_messageHandler);
    RegisterFormattingAndSymbolHandlers(*this, *m_messageHandler);
}

std::optional<uint64_t> Server::ClosedDiagnosticRevision(const std::string& uriStr, const std::string* source) const
{
    std::lock_guard lock(m_closureMutex);
    const auto entry = m_closureDocuments.find(uriStr);
    if (entry == m_closureDocuments.end() || entry->second.text.get() != source)
    {
        return std::nullopt;
    }
    return entry->second.revision;
}

std::optional<lsp::requests::TextDocument_Diagnostic::Result>
Server::TryGetCachedDiagnosticReport(const std::string& uriStr, size_t currentHash, int currentVersion,
                                     uint64_t closedRevision, const std::optional<std::string>& previousResultId)
{
    std::unique_lock closureLock(m_closureMutex, std::defer_lock);
    if (closedRevision != 0)
    {
        closureLock.lock();
        if (!IsClosedRevisionCurrentLocked(uriStr, closedRevision))
        {
            return std::nullopt;
        }
    }
    std::lock_guard<std::mutex> lock(m_diagnosticsPipeline->GetCacheMutexRef());
    const auto it = m_diagnosticsPipeline->GetCacheRef().find(uriStr);
    if (it == m_diagnosticsPipeline->GetCacheRef().end())
    {
        return std::nullopt;
    }

    const bool isCurrent = it->second.configRevision == m_sessionCoordinator->GetConfigRevision() &&
                           it->second.version == currentVersion && it->second.textHash == currentHash &&
                           it->second.closedRevision == closedRevision;
    if (!isCurrent)
    {
        return std::nullopt;
    }

    if (previousResultId.has_value() && *previousResultId == it->second.resultId)
    {
        lsp::RelatedUnchangedDocumentDiagnosticReport unchanged;
        unchanged.resultId = it->second.resultId;
        return unchanged;
    }

    lsp::RelatedFullDocumentDiagnosticReport full;
    full.resultId = it->second.resultId;
    full.items = it->second.items;
    return full;
}

lsp::requests::TextDocument_Diagnostic::Result
Server::HandleRequestsTextDocument_Diagnostic(lsp::requests::TextDocument_Diagnostic::Params&& params)
{
    if (!m_sessionCoordinator->GetClientPullsDiagnostics())
    {
        throw lsp::RequestError(lsp::MessageError::MethodNotFound, "Pull diagnostics are disabled");
    }

    const std::string uriStr = DocumentKey(params.textDocument.uri.toString());

    if (!m_config.modules.empty() && !GetDocumentStore().IsOpen(uriStr))
    {
        const std::string path = CanonicalPathFromUri(uriStr);
        if (ClaimFor(path).owner == nullptr)
        {
            m_diagnosticsPipeline->InvalidateDiagnostics(uriStr);
            lsp::RelatedFullDocumentDiagnosticReport emptyReport;
            emptyReport.items = {};
            return emptyReport;
        }
    }

    const auto current = FindDocumentText(uriStr);
    const int currentVersion = GetDocumentVersion(uriStr);
    const bool isClosedScript =
        currentVersion < 0 && !utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension);
    const auto closedRevision =
        isClosedScript && current ? ClosedDiagnosticRevision(uriStr, current.get()) : std::optional<uint64_t>{0};

    if (current && closedRevision && m_parser)
    {
        auto prepared = PrepareAnalysisSource(uriStr, *current);
        const size_t currentHash = std::hash<std::string>{}(prepared.text);
        if (auto cached = TryGetCachedDiagnosticReport(uriStr, currentHash, currentVersion, *closedRevision,
                                                       params.previousResultId))
        {
            return std::move(*cached);
        }

        AnalyzeDocument({
            .uriStr = uriStr,
            .text = std::move(prepared.text),
            .parser = *m_parser,
            .version = currentVersion,
            .generation = GetDocumentStore().GetGeneration(uriStr),
            .configRevision = m_sessionCoordinator->GetConfigRevision(),
            .closedRevision = *closedRevision,
            .preprocessing = std::move(prepared.preprocessing),
        });

        if (auto fresh = TryGetCachedDiagnosticReport(uriStr, currentHash, currentVersion, *closedRevision,
                                                      params.previousResultId))
        {
            return std::move(*fresh);
        }
    }

    lsp::json::Object retrigger;
    retrigger["retriggerRequest"] = true;

    throw lsp::RequestError(lsp::MessageError::ServerCancelled,
                            "Diagnostics for this document are still being computed",
                            lsp::json::Value(std::move(retrigger)));
}

} // namespace angel_lsp
