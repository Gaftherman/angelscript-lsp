#include "analysis/index/CallGraph.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "lsp/Server.h"
#include "parser/SyntaxTree.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp
{
ParseAndCollectResult Server::ParseAndCollectSymbols(AnalyzeDocumentRequest& req,
                                                     angel_lsp::analysis::SymbolTable& staging,
                                                     SyntaxRevisionCache* syntaxCache)
{
    const auto open = !req.fromScheduler && req.generation != 0 ? GetDocumentStore().GetDocument(req.uriStr) : nullptr;
    const bool hasExactTree =
        open && open->tree && open->generation == req.generation && open->ParsedSource() == req.text;
    utils::HighResTimer colTimer;
    auto [syntax, extractedNodes, parseMs] =
        ResolveSyntaxRevision({.key = {req.uriStr, req.text, req.generation, req.closedRevision},
                               .parser = req.parser,
                               .tree = std::move(req.treeCopy),
                               .exactTreeToCopy = hasExactTree ? &open->tree : nullptr},
                              syntaxCache);
    if (!syntax)
    {
        return {.parseMs = parseMs};
    }
    auto closed = FindClosedAnalysisInputs(req);
    if (closed.indexedSyntax)
    {
        LogInfo(fmt::format("[Analysis] Reused closed indexed symbols for file: {}", req.uriStr));
        auto diagnostics = closed.indexedSyntax->diagnostics;
        return {std::move(syntax), std::move(diagnostics), parseMs, (colTimer.ElapsedMs() - parseMs),
                extractedNodes,    std::move(closed)};
    }

    auto diagnostics =
        m_symbolCollector->CollectSymbolsWithIndex({req.uriStr, req.text, m_i18n.get()}, syntax->index, staging);
    if (syntax->index.HasSyntaxErrors())
    {
        m_symbolTable.RetainLastKnownGoodTypes(req.uriStr, staging);
    }
    return {std::move(syntax), std::move(diagnostics), parseMs, (colTimer.ElapsedMs() - parseMs), extractedNodes};
}

void Server::LogAnalysisProfile(std::string_view prefix, std::string_view uriStr,
                                const AnalysisTimingProfile& profile) const
{
    LogInfo(fmt::format("{} Finished analysis for file: {} in {} (Snapshot: {}, Parse: {}, Collector: {}, "
                        "Scopes: {}, Checkers: {})",
                        prefix, uriStr, utils::FormatDuration(profile.totalMs),
                        utils::FormatDuration(profile.snapshotMs), utils::FormatDuration(profile.parseMs),
                        utils::FormatDuration(profile.colMs), utils::FormatDuration(profile.scopeMs),
                        utils::FormatDuration(profile.checkMs)));
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {} (Snapshot: {}, Parse: {}, Collector: {}, "
                        "Scopes: {}, Checkers: {}, AST extraction nodes: {})",
                        uriStr, utils::FormatDuration(profile.totalMs), utils::FormatDuration(profile.snapshotMs),
                        utils::FormatDuration(profile.parseMs), utils::FormatDuration(profile.colMs),
                        utils::FormatDuration(profile.scopeMs), utils::FormatDuration(profile.checkMs),
                        profile.extractedNodes));
}

const analysis::SymbolTable* Server::PrepareAnalysisSnapshot(std::string_view uriStr,
                                                             const analysis::SymbolTable* staging,
                                                             std::unique_ptr<analysis::SymbolTable>& outSnapshot) const
{
    const auto moduleUris = ComputeModuleClosureUris(uriStr);
    const bool sameInterface =
        !staging || m_symbolTable.ComputeDocumentInterfaceHash(std::string(uriStr)) == staging->ComputeDocumentInterfaceHash(std::string(uriStr));
    if (sameInterface && (moduleUris.empty() || m_config.modules.empty() ||
                          m_symbolTable.ContainsOnlyFiles(moduleUris, m_config.info.predefinedFileExtension)))
    {
        return &m_symbolTable;
    }
    const auto predefinedBase = GetWorkspaceStore().GetPredefinedBaseTable();
    outSnapshot = m_symbolTable.CreateAnalysisSnapshot({
        .uriStr = std::string(uriStr),
        .staging = staging,
        .allowedModuleFileUris = &moduleUris,
        .predefinedExtension = m_config.info.predefinedFileExtension,
        .predefinedBase = predefinedBase.get(),
    });
    return outSnapshot.get();
}

static void LogAnalysisProfileAndClosure(const Server& server, std::string_view uriStr, double closureMs,
                                         const AnalysisTimingProfile& profile)
{
    server.LogAnalysisProfile("[Analysis]", uriStr, profile);
    if (closureMs > 0.05)
    {
        server.LogInfo(
            fmt::format("[Analysis Breakdown] File: {} | Closure: {}", uriStr, utils::FormatDuration(closureMs)));
    }
}

std::optional<ParseAndCollectResult> Server::PrepareAnalysisDocument(AnalyzeDocumentRequest& req,
                                                                     angel_lsp::analysis::SymbolTable& staging)
{
    LogInfo(fmt::format("[Analysis] Starting background analysis for file: {}", req.uriStr));

    if (m_config.features.enablePredefinedLoader && !IsPredefinedReady())
    {
        WaitForPredefinedReady(std::chrono::milliseconds(5000));
    }

    auto parsed = ParseAndCollectSymbols(req, staging, req.fromScheduler ? &m_workerSyntax : &m_protocolSyntax);
    if (!parsed.syntax)
    {
        return std::nullopt;
    }

    if (m_config.features.enablePredefinedLoader && !IsPredefinedReady())
    {
        CommitSyntaxOnlyResults(req, std::move(parsed.diagnostics));
        return std::nullopt;
    }

    if (IsAnalysisRequestStale(req))
    {
        return std::nullopt;
    }

    return parsed;
}

ClosedAnalysisInputs Server::FindClosedAnalysisInputs(const AnalyzeDocumentRequest& req) const
{
    if (req.closedRevision == 0)
    {
        return {};
    }
    std::lock_guard lock(m_closureMutex);
    const auto entry = m_closureDocuments.find(req.uriStr);
    if (entry == m_closureDocuments.end() || entry->second.revision != req.closedRevision ||
        !entry->second.indexedSyntax || *entry->second.indexedSyntax->source != req.text)
    {
        return {};
    }
    return {entry->second.indexedSyntax, entry->second.lexicalScopes};
}

std::pair<std::shared_ptr<analysis::Scope>, std::vector<analysis::CallSite>>
Server::CollectScopesAndCalls(const AnalyzeDocumentRequest& req, const ParseAndCollectResult& parsed)
{
    const auto& index = parsed.syntax->index;
    std::shared_ptr<analysis::Scope> scopes;
    if (const auto& extracted = parsed.closed.lexicalScopes)
    {
        scopes = analysis::CopyLexicalScopeTree(*extracted);
        LogInfo(fmt::format("[Analysis] Reused closed lexical scopes for file: {}", req.uriStr));
    }
    else if (m_localScopeCollector)
    {
        scopes = m_localScopeCollector->CollectScopesFromIndex(index, req.text);
    }

    std::vector<analysis::CallSite> calls;
    if (!parsed.closed.indexedSyntax)
    {
        calls =
            scopes ? analysis::CollectCallsFromScopeTree(*scopes) : analysis::CollectCallsFromIndex(index, req.text);
    }
    return {std::move(scopes), std::move(calls)};
}

void Server::AnalyzeNormalDocument(AnalyzeDocumentRequest req, const utils::HighResTimer& totalTimer)
{
    angel_lsp::analysis::SymbolTable staging;
    auto parsedOpt = PrepareAnalysisDocument(req, staging);
    if (!parsedOpt)
    {
        return;
    }
    auto parsed = std::move(*parsedOpt);

    const auto& nodeIndex = parsed.syntax->index;
    auto* replacement = parsed.closed.indexedSyntax ? nullptr : &staging;
    utils::HighResTimer closureTimer;
    if (replacement)
    {
        IndexModuleClosure(req.uriStr, req.fromScheduler ? &m_workerSyntax : &m_protocolSyntax);
    }
    const double closureMs = closureTimer.ElapsedMs();

    utils::HighResTimer snapshotTimer;
    std::unique_ptr<analysis::SymbolTable> analysisSnapshot;
    const analysis::SymbolTable* activeTable = PrepareAnalysisSnapshot(req.uriStr, replacement, analysisSnapshot);
    const double snapshotMs = snapshotTimer.ElapsedMs();

    utils::HighResTimer scopeTimer;
    auto [scopeRoot, calls] = CollectScopesAndCalls(req, parsed);
    const double scopeMs = scopeTimer.ElapsedMs();

    if (!m_config.modules.empty() && ClaimFor(CanonicalPathFromUri(req.uriStr)).owner == nullptr)
    {
        LogAnalysisProfileAndClosure(*this, req.uriStr, closureMs,
                                     AnalysisTimingProfile{totalTimer.ElapsedMs(), snapshotMs, parsed.parseMs,
                                                           parsed.colMs, scopeMs, 0.0, parsed.extractedNodes});
        CommitAnalysisResults({req.uriStr,
                               req.version,
                               req.generation,
                               req.configRevision,
                               replacement,
                               std::move(scopeRoot),
                               std::move(calls),
                               {},
                               req.text,
                               req.closedRevision,
                               req.preprocessing});
        return;
    }

    const auto& preprocessing = *req.preprocessing;
    auto request = BuildAnalysisRequest(req.uriStr, req.text, &nodeIndex, preprocessing, activeTable);
    request.scopeRoot = scopeRoot;
    request.mutableScopeRoot = scopeRoot.get();
    request.nodeIndex = &nodeIndex;
    request.callBindingRevision = analysis::CallBindingRevision{req.generation, req.version, req.configRevision};

    double checkMs = 0.0;
    auto semanticDiagnostics = RunDocumentSemantics(std::move(request), checkMs, req.fromScheduler);

    parsed.diagnostics.insert(parsed.diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());
    AppendIncludeDiagnostics(req.uriStr, req.text, preprocessing.excluded, parsed.diagnostics);

    LogAnalysisProfileAndClosure(*this, req.uriStr, closureMs,
                                 AnalysisTimingProfile{totalTimer.ElapsedMs(), snapshotMs, parsed.parseMs, parsed.colMs,
                                                       scopeMs, checkMs, parsed.extractedNodes});

    CommitAnalysisResults({req.uriStr, req.version, req.generation, req.configRevision, replacement,
                           std::move(scopeRoot), std::move(calls), std::move(parsed.diagnostics), req.text,
                           req.closedRevision, req.preprocessing});
}

void Server::AnalyzeDocument(AnalyzeDocumentRequest req)
{
    utils::HighResTimer totalTimer;

    if (IsAnalyzeDocumentStale(req))
    {
        return;
    }

    const bool isPredefined = angel_lsp::utils::IsPredefinedFile(req.uriStr, m_config.info.predefinedFileExtension);
    if (isPredefined)
    {
        AnalyzePredefinedDocument(std::move(req), totalTimer);
    }
    else
    {
        if (!IsPredefinedReady())
        {
            WaitForPredefinedReady(std::chrono::milliseconds(2000));
        }
        AnalyzeNormalDocument(std::move(req), totalTimer);
    }
}
} // namespace angel_lsp
