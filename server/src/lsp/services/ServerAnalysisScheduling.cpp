#include "lsp/Server.h"
#include "parser/SyntaxTree.h"
#include "utils/Utils.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{
void Server::ReanalyseOpenDocuments()
{
    const auto docs = GetDocumentStore().GetAllDocuments();
    for (const auto& doc : docs)
    {
        if (!doc || angel_lsp::utils::IsPredefinedFile(doc->uri, m_config.info.predefinedFileExtension))
        {
            continue;
        }
        IndexModuleClosure(doc->uri);
        ScheduleAnalysis(ScheduleAnalysisRequest{.uriStr = doc->uri,
                                                 .text = doc->text,
                                                 .force = true,
                                                 .version = doc->version,
                                                 .generation = doc->generation});
    }
}

static bool MatchesOpenRevision(const ScheduleAnalysisRequest& request, const document::Document& open)
{
    return request.text == open.text && (request.version < 0 || request.version == open.version) &&
           (request.generation == 0 || request.generation == open.generation);
}

std::optional<ScheduleRequest> Server::MakeScheduleRequest(ScheduleAnalysisRequest req) const
{
    const auto open = GetDocumentStore().GetDocument(req.uriStr);
    uint64_t closedRevision = 0;
    if (open)
    {
        if (!MatchesOpenRevision(req, *open))
        {
            return std::nullopt;
        }
        req.version = open->version;
        req.generation = open->generation;
    }
    else
    {
        if (req.generation != 0 || req.version >= 0)
        {
            return std::nullopt;
        }
        std::lock_guard lock(m_closureMutex);
        const auto entry = m_closureDocuments.find(req.uriStr);
        if (entry == m_closureDocuments.end() || !entry->second.text || *entry->second.text != req.text)
        {
            return std::nullopt;
        }
        closedRevision = entry->second.revision;
    }

    const uint64_t configRevision = m_sessionCoordinator->GetConfigRevision();
    auto prepared = req.preparedSource ? std::move(*req.preparedSource) : PrepareAnalysisSource(req.uriStr, req.text);
    parser::TreePtr tree = parser::MakeTreePtr();
    if (open && open->tree && open->ParsedSource() == prepared.text)
    {
        tree = parser::CopyTree(open->tree.get());
    }
    return ScheduleRequest{std::move(req.uriStr),
                           std::move(prepared.text),
                           req.force,
                           std::move(tree),
                           req.version,
                           req.generation,
                           configRevision,
                           closedRevision,
                           std::move(prepared.preprocessing)};
}

void Server::ScheduleAnalysis(ScheduleAnalysisRequest req)
{
    if (auto scheduled = MakeScheduleRequest(std::move(req)))
    {
        m_diagnosticsPipeline->Schedule(std::move(*scheduled));
    }
}

void Server::ScheduleAnalysis(std::string_view uriStr, std::string_view text, bool force)
{
    ScheduleAnalysis(ScheduleAnalysisRequest{.uriStr = std::string(uriStr), .text = std::string(text), .force = force});
}

void Server::ScheduleAnalysisImmediate(ScheduleAnalysisRequest req)
{
    if (auto scheduled = MakeScheduleRequest(std::move(req)))
    {
        m_diagnosticsPipeline->ScheduleImmediate(std::move(*scheduled));
    }
}

bool Server::IsClosedRevisionCurrentLocked(std::string_view uri, uint64_t revision) const
{
    const auto entry = m_closureDocuments.find(uri);
    return entry != m_closureDocuments.end() && entry->second.revision == revision;
}

bool Server::IsCommitAnalysisStale(const CommitAnalysisRequest& req) const
{
    if (req.generation > 0 && !GetDocumentStore().IsCurrent(req.uriStr, req.generation, req.version))
    {
        return true;
    }
    if (!angel_lsp::utils::IsPredefinedFile(req.uriStr, m_config.info.predefinedFileExtension) &&
        req.configRevision != m_sessionCoordinator->GetConfigRevision())
    {
        return true;
    }
    if (m_diagnosticsPipeline->IsCancelled(req.uriStr))
    {
        return true;
    }

    const int currentVer = GetDocumentVersion(req.uriStr);
    if (currentVer >= 0 && req.version >= 0 && req.version != currentVer)
    {
        return true;
    }
    if (currentVer >= 0 && req.version < 0)
    {
        return true;
    }
    return false;
}

bool Server::IsAnalysisRequestStale(const AnalyzeDocumentRequest& req) const
{
    if (!utils::IsPredefinedFile(req.uriStr, m_config.info.predefinedFileExtension) &&
        req.configRevision != m_sessionCoordinator->GetConfigRevision())
    {
        return true;
    }
    if (req.generation > 0 && !GetDocumentStore().IsCurrent(req.uriStr, req.generation, req.version))
    {
        return true;
    }
    if (req.closedRevision != 0)
    {
        std::lock_guard lock(m_closureMutex);
        if (!IsClosedRevisionCurrentLocked(req.uriStr, req.closedRevision))
        {
            return true;
        }
    }
    return m_diagnosticsPipeline->IsCancelled(req.uriStr);
}

bool Server::CommitAnalysisResults(CommitAnalysisRequest req)
{
    if (m_onBeforeCommitHook)
    {
        m_onBeforeCommitHook(req.uriStr, req.version, req.generation);
    }

    std::lock_guard<std::mutex> lock(m_lifecycleMutex);

    std::unique_lock closedLock(m_closureMutex, std::defer_lock);
    if (req.closedRevision != 0)
    {
        closedLock.lock();
        if (!IsClosedRevisionCurrentLocked(req.uriStr, req.closedRevision))
        {
            return false;
        }
    }

    if (IsCommitAnalysisStale(req))
    {
        return false;
    }

    if (req.staging)
    {
        m_symbolTable.ReplaceDocumentSymbols(req.uriStr, std::move(*req.staging));
        m_callGraph.SetDocumentCalls(req.uriStr, std::move(req.calls));
    }

    if (req.scopeRoot)
    {
        m_scopeIndex.SetScopeTree(req.uriStr, std::move(req.scopeRoot));
    }
    else
    {
        m_scopeIndex.ClearDocument(req.uriStr);
    }

    if (closedLock.owns_lock())
    {
        auto& closed = m_closureDocuments.at(req.uriStr);
        closed.lexicalScopes.reset();
        if (req.staging)
        {
            closed.indexedSyntax.reset();
        }
    }

    PublishDiagnostics(PublishDiagnosticsRequest{
        .uriStr = req.uriStr,
        .text = req.text,
        .diagnostics = std::move(req.diagnostics),
        .version = req.version,
        .generation = req.generation,
        .preprocessing = std::move(req.preprocessing),
        .closedRevision = req.closedRevision,
    });
    return true;
}

void Server::CommitSyntaxOnlyResults(const AnalyzeDocumentRequest& req, std::vector<analysis::Diagnostic> diags)
{
    CommitAnalysisResults({.uriStr = req.uriStr,
                           .version = req.version,
                           .generation = req.generation,
                           .configRevision = req.configRevision,
                           .staging = nullptr,
                           .scopeRoot = nullptr,
                           .calls = {},
                           .diagnostics = std::move(diags),
                           .text = req.text,
                           .closedRevision = req.closedRevision,
                           .preprocessing = req.preprocessing});
}
} // namespace angel_lsp
