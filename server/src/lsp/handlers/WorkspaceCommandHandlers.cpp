#include "analysis/formatting/PredefinedStubFormatter.h"
#include "lsp/Server.h"
#include "parser/AngelScriptParser.h"
#include "utils/Utils.h"

#include <fstream>
#include <mutex>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{

lsp::requests::Workspace_Diagnostic::Result
Server::HandleRequestsWorkspace_Diagnostic(lsp::requests::Workspace_Diagnostic::Params&& params)
{
    if (!m_sessionCoordinator->GetClientPullsDiagnostics())
    {
        return lsp::WorkspaceDiagnosticReport{};
    }

    // What the client already holds, so an unedited document can be answered with its id alone.
    ankerl::unordered_dense::map<std::string, std::string> known;
    for (const auto& previous : params.previousResultIds)
        known[DocumentKey(previous.uri.toString())] = previous.value;

    lsp::WorkspaceDiagnosticReport report;

    std::lock_guard<std::mutex> lock(m_diagnosticsPipeline->GetCacheMutexRef());
    for (const auto& [uriStr, snapshot] : m_diagnosticsPipeline->GetCacheRef())
    {
        // Out under the client's own spelling, for the same reason PublishDiagnostics does it:
        // the key is canonical and the client matches these against its own URIs.
        const std::string outgoingUri = GetDocumentStore().GetClientUri(uriStr);

        if (const auto it = known.find(uriStr); it != known.end() && it->second == snapshot.resultId)
        {
            lsp::WorkspaceUnchangedDocumentDiagnosticReport unchanged;
            unchanged.uri = lsp::DocumentUri(lsp::Uri::parse(outgoingUri));
            unchanged.version = (snapshot.version >= 0) ? lsp::NullOr<int>(snapshot.version) : nullptr;
            unchanged.resultId = snapshot.resultId;
            report.items.push_back(std::move(unchanged));
            continue;
        }

        lsp::WorkspaceFullDocumentDiagnosticReport full;
        full.uri = lsp::DocumentUri(lsp::Uri::parse(outgoingUri));
        full.version = (snapshot.version >= 0) ? lsp::NullOr<int>(snapshot.version) : nullptr;
        full.resultId = snapshot.resultId;
        full.items = snapshot.items;
        report.items.push_back(std::move(full));
    }

    return report;
}

lsp::requests::Workspace_ExecuteCommand::Result
Server::ExecuteFormatPredefinedStub(const std::optional<lsp::Array<lsp::LSPAny>>& args)
{
    if (!args || args->empty() || !args->front().isString())
    {
        LogError("angelscript.formatPredefinedStub needs the stub's URI as its argument");
        return lsp::Null{};
    }

    const std::string uriStr = args->front().string();
    std::string text;

    if (const auto doc = LookupOpenDocument(uriStr); doc && doc->text)
    {
        text = *doc->text;
    }
    else
    {
        const std::string path = CanonicalPathFromUri(uriStr);
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            LogError(fmt::format("Cannot open predefined file to format: {}", path));
            return lsp::Null{};
        }
        text.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    angel_lsp::parser::AngelScriptParser formatterParser(m_logger.get());
    std::string formatted = angel_lsp::analysis::formatting::FormatPredefinedStub(text, formatterParser);

    const bool changed = (formatted != text);
    LogInfo(fmt::format("Formatted predefined stub {}: {}", uriStr, changed ? "rewritten" : "already formatted"));

    lsp::json::Object answer;
    answer["changed"] = lsp::json::Value(changed);
    answer["text"] = lsp::json::Value(std::move(formatted));
    return lsp::json::Value(std::move(answer));
}

lsp::requests::Workspace_ExecuteCommand::Result Server::ExecuteListPredefinedStubs() const
{
    lsp::json::Object answer;
    lsp::json::Array paths;
    std::string effective;

    {
        std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
        for (const auto& path : m_discoveredPredefined)
        {
            paths.push_back(lsp::json::Value(std::string(path)));
        }
        effective = m_effectivePredefined;
    }

    answer["stubs"] = std::move(paths);
    answer["active"] = std::move(effective);
    answer["merging"] = (m_config.activePredefined == "all");

    return lsp::json::Value(std::move(answer));
}

lsp::requests::Workspace_ExecuteCommand::Result
Server::HandleRequestsWorkspace_ExecuteCommand(lsp::requests::Workspace_ExecuteCommand::Params&& params)
{
    if (params.command == "angelscript.rescanWorkspace")
    {
        RestartWorkspaceScan();
        return lsp::Null{};
    }

    if (params.command == "angelscript.formatPredefinedStub")
    {
        return ExecuteFormatPredefinedStub(params.arguments);
    }

    if (params.command == "angelscript.listPredefinedStubs")
    {
        return ExecuteListPredefinedStubs();
    }

    throw lsp::RequestError(lsp::MessageError::InvalidParams, "Unknown command: " + params.command);
}

} // namespace angel_lsp
