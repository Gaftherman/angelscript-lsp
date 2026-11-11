#include "analysis/index/CallGraph.h"
#include "analysis/index/NodeIndex.h"
#include "lsp/Server.h"
#include "parser/SyntaxTree.h"
#include "utils/Constants.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <algorithm>
#include <spdlog/fmt/fmt.h>

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

ankerl::unordered_dense::set<std::string> Server::ComputeModuleClosureUris(const std::string& uriStr) const
{
    ankerl::unordered_dense::set<std::string> moduleFileUris;
    moduleFileUris.insert(uriStr);
    const std::string ownPath = CanonicalPathFromUri(uriStr);
    if (ownPath.empty())
    {
        return moduleFileUris;
    }

    const auto closurePaths = ComputeModuleClosure(ownPath);
    std::lock_guard<std::mutex> lock(m_closureMutex);
    for (const auto& path : closurePaths)
    {
        if (path == ownPath)
        {
            continue;
        }

        const auto indexed = m_indexedUriByPath.find(path);
        moduleFileUris.insert(indexed != m_indexedUriByPath.end() ? indexed->second : UriFromPath(path));
    }
    return moduleFileUris;
}

angel_lsp::analysis::SemanticAnalysisRequest
Server::BuildAnalysisRequest(const std::string& uriStr, const std::string& text,
                             const analysis::NodeIndex* nodeIndex,
                             const utils::PreprocessorScan& preprocessing,
                             const angel_lsp::analysis::SymbolTable* customSymbolTable) const
{
    angel_lsp::analysis::SemanticAnalysisRequest request{customSymbolTable ? *customSymbolTable : m_symbolTable, uriStr,
                                                         std::string(m_config.info.predefinedFileExtension),
                                                         m_i18n.get()};

    request.typeConfig = &m_config.types;
    request.engineProperties = &m_config.engine;
    request.moduleFileUris = ComputeModuleClosureUris(uriStr);
    request.moduleContext = ModuleContextFor(uriStr);

    request.diagnostics = &m_config.diagnostics;
    request.engineRules = &m_config.engineRules;
    request.severityOverrides = m_diagnosticsPipeline->GetDiagnosticSeverities().empty()
                                    ? nullptr
                                    : &m_diagnosticsPipeline->GetDiagnosticSeverities();
    request.enableTypeConversionChecks = m_config.features.enableTypeConversionChecks;
    request.enableCommentSuppressions = m_config.features.enableCommentSuppressions;
    request.enableMetadata = m_config.features.enableMetadata;
    request.scopeRoot = m_scopeIndex.GetRoot(uriStr);
    request.scopeIndex = &m_scopeIndex;
    request.sourceCode = text;
    request.nodeIndex = nodeIndex;

    request.excludedLineRanges = preprocessing.excluded;
    request.unsupportedDirectives = preprocessing.unsupported;

    request.pragmaSeverity = m_config.pragmaMode == config::ServerConfig::PragmaMode::Error
                                 ? angel_lsp::analysis::DiagnosticSeverity::Error
                                 : angel_lsp::analysis::DiagnosticSeverity::Hint;

    return request;
}

std::vector<angel_lsp::analysis::Diagnostic> Server::ReplaceSymbolsFromTree(const std::string& uriStr,
                                                                            const std::string& text, TSTree* tree,
                                                                            bool* outInterfaceChanged,
                                                                            const analysis::NodeIndex* index)
{
    angel_lsp::analysis::SymbolTable staging;
    const analysis::SymbolCollectRequest request{uriStr, text, m_i18n.get()};
    auto diagnostics = index ? m_symbolCollector->CollectSymbolsWithIndex(request, *index, staging)
                             : m_symbolCollector->CollectSymbolsWithTree(request, tree, staging);
    if (tree && ts_node_has_error(ts_tree_root_node(tree)))
    {
        m_symbolTable.RetainLastKnownGoodTypes(uriStr, staging);
    }
    if (outInterfaceChanged)
    {

        const uint64_t newHash = staging.ComputeDocumentInterfaceHash(uriStr);
        uint64_t oldHash = m_symbolTable.ComputeDocumentInterfaceHash(uriStr);
        {
            std::lock_guard<std::mutex> lock(m_closureMutex);
            if (const auto it = m_lastSavedInterfaceHash.find(uriStr); it != m_lastSavedInterfaceHash.end())
            {
                oldHash = it->second;
            }
            m_lastSavedInterfaceHash[uriStr] = newHash;
        }

        *outInterfaceChanged = (oldHash != newHash);
    }
    m_symbolTable.ReplaceDocumentSymbols(uriStr, std::move(staging));
    return diagnostics;
}

std::vector<angel_lsp::analysis::Diagnostic>
Server::ReplaceSymbolsFromSource(const std::string& uriStr, const std::string& text,
                                 angel_lsp::parser::AngelScriptParser& parser)
{
    angel_lsp::analysis::SymbolTable staging;
    auto diagnostics = m_symbolCollector->CollectSymbols({uriStr, text, m_i18n.get()}, parser, staging);
    m_symbolTable.ReplaceDocumentSymbols(uriStr, std::move(staging));
    return diagnostics;
}

bool Server::ShouldSkipScopeAnalysis(const CollectScopesRequest& request)
{
    if (request.source.text.size() > angel_lsp::constants::limits::MaxAnalysedDocumentBytes)
    {
        LogWarning(fmt::format("Skipping analysis of {}: {} bytes exceeds the {} byte limit. Navigation still works.",
                               request.uriStr, request.source.text.size(),
                               angel_lsp::constants::limits::MaxAnalysedDocumentBytes));

        m_scopeIndex.ClearDocument(request.uriStr);
        m_callGraph.ClearDocument(request.uriStr);
        if (request.outScopeMs)
        {
            *request.outScopeMs = 0.0;
        }
        if (request.outCheckMs)
        {
            *request.outCheckMs = 0.0;
        }
        return true;
    }

    if (angel_lsp::utils::IsPredefinedFile(request.uriStr, m_config.info.predefinedFileExtension))
    {
        if (request.outScopeMs)
        {
            *request.outScopeMs = 0.0;
        }
        if (request.outCheckMs)
        {
            *request.outCheckMs = 0.0;
        }
        return true;
    }

    return false;
}

std::vector<angel_lsp::analysis::Diagnostic> Server::CollectScopesAndAnalyze(const CollectScopesRequest& request)
{
    if (ShouldSkipScopeAnalysis(request))
    {
        return {};
    }

    std::shared_ptr<angel_lsp::analysis::Scope> scopeRoot;
    utils::HighResTimer scopeTimer;
    if (request.tree)
    {
        const TSNode root = request.nodeIndex.Root();
        scopeRoot = m_localScopeCollector->CollectScopesFromTree(root, request.source.text, &request.nodeIndex);
        if (scopeRoot)
        {
            m_callGraph.SetDocumentCalls(request.uriStr, analysis::CollectCallsFromScopeTree(*scopeRoot));
        }
        else
        {
            m_callGraph.SetDocumentCalls(request.uriStr, analysis::CollectCalls(root, request.source.text));
        }
    }
    else
    {
        m_scopeIndex.ClearDocument(request.uriStr);
        m_callGraph.ClearDocument(request.uriStr);
    }
    if (request.outScopeMs)
    {
        *request.outScopeMs = scopeTimer.ElapsedMs();
    }

    auto analysisReq =
        BuildAnalysisRequest(request.uriStr, request.source.text, &request.nodeIndex, *request.source.preprocessing);
    analysisReq.scopeRoot = scopeRoot;
    analysisReq.mutableScopeRoot = scopeRoot.get();
    analysisReq.nodeIndex = &request.nodeIndex;
    analysisReq.callBindingRevision = request.callBindingRevision;
    analysisReq.mixinCache = &m_serverMixinCache;

    utils::HighResTimer checkTimer;
    auto diagnostics = m_semanticAnalyzer->Analyze(analysisReq);
    if (request.outCheckMs)
    {
        *request.outCheckMs = checkTimer.ElapsedMs();
    }

    if (scopeRoot)
    {
        m_scopeIndex.SetScopeTree(request.uriStr,
                                  std::shared_ptr<const angel_lsp::analysis::Scope>(std::move(scopeRoot)));
    }

    return diagnostics;
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

void Server::ScheduleAnalysis(const std::string& uriStr, const std::string& text, bool force)
{
    ScheduleAnalysis(ScheduleAnalysisRequest{.uriStr = uriStr, .text = text, .force = force});
}

void Server::ScheduleAnalysisImmediate(ScheduleAnalysisRequest req)
{
    if (auto scheduled = MakeScheduleRequest(std::move(req)))
    {
        m_diagnosticsPipeline->ScheduleImmediate(std::move(*scheduled));
    }
}

bool Server::IsClosedRevisionCurrentLocked(const std::string& uri, uint64_t revision) const
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

bool Server::CommitAnalysisResults(CommitAnalysisRequest req)
{
    if (m_onBeforeCommitHook)
    {
        m_onBeforeCommitHook(req.uriStr, req.version, req.generation);
    }

    std::lock_guard<std::mutex> lock(m_lifecycleMutex);

    // The disk revision stays valid until the related symbols, occurrences and diagnostics are published.
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

bool Server::IsAnalyzeDocumentStale(const AnalyzeDocumentRequest& req) const
{
    if (IsAnalysisRequestStale(req))
    {
        return true;
    }

    const int currentVersion = GetDocumentVersion(req.uriStr);
    if (currentVersion >= 0 && req.version >= 0 && req.version != currentVersion)
    {
        return true;
    }
    if (currentVersion >= 0 && req.version < 0)
    {
        return true;
    }
    return false;
}

void Server::StorePredefinedSnapshot(const AnalyzeDocumentRequest& req, const std::string& analysisText,
                                     const parser::TreePtr& tree, bool contributes)
{
    if (contributes)
    {
        ClaimPredefinedFile(req.uriStr, true);
        m_predefinedManager.SetDocumentText(req.uriStr, analysisText);
        if (tree)
        {
            GetDocumentStore().SetTree(req.uriStr, parser::CopyTree(tree.get()));
            auto updatedDoc = std::make_shared<const document::Document>(
                document::DocumentSnapshot{req.uriStr, analysisText, req.version, req.generation},
                parser::CopyTree(tree.get()));
            m_predefinedManager.SetPreindexedDocument(req.uriStr, updatedDoc);
            if (m_workspaceStore)
            {
                m_workspaceStore->SetPreindexedDocument(req.uriStr, updatedDoc);
            }
        }
    }
    else
    {
        m_predefinedManager.RemoveStub(req.uriStr);
    }
}

bool Server::TryZeroCopyPredefinedAnalysis(const AnalyzeDocumentRequest& req, const std::string& analysisText,
                                           const utils::HighResTimer& totalTimer)
{
    auto preindexedDoc = m_workspaceStore ? m_workspaceStore->GetPreindexedDocument(req.uriStr)
                                          : m_predefinedManager.GetPreindexedDocument(req.uriStr);
    if (!preindexedDoc)
    {
        preindexedDoc = m_predefinedManager.GetPreindexedDocument(req.uriStr);
    }
    if (!preindexedDoc || !angel_lsp::utils::TextContentMatchesIgnoringLineEndings(preindexedDoc->text, analysisText))
    {
        return false;
    }
    const double totalMs = totalTimer.ElapsedMs();
    LogInfo(
        fmt::format("[Predefined Zero-Copy Fast Path] File: {} content unchanged; linked pre-indexed snapshot in {}",
                    req.uriStr, utils::FormatDuration(totalMs)));
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {} (Parse: 0.00 ms, Collector: 0.00 ms, "
                        "Scopes: 0.00 ms, Checkers: 0.00 ms)",
                        req.uriStr, utils::FormatDuration(totalMs)));
    CommitAnalysisResults({
        .uriStr = req.uriStr,
        .version = req.version,
        .generation = req.generation,
        .configRevision = req.configRevision,
        .staging = nullptr,
        .scopeRoot = nullptr,
        .calls = {},
        .diagnostics = {},
        .text = req.text,
        .closedRevision = req.closedRevision,
    });
    return true;
}

void Server::AnalyzePredefinedDocument(AnalyzeDocumentRequest req, const utils::HighResTimer& totalTimer)
{
    LogInfo(fmt::format("[Analysis] Starting predefined analysis for file: {}", req.uriStr));
    const std::string& analysisText = req.text;
    if (TryZeroCopyPredefinedAnalysis(req, analysisText, totalTimer))
    {
        return;
    }
    utils::HighResTimer parseTimer;
    parser::TreePtr tree = req.treeCopy ? std::move(req.treeCopy) : parser::MakeTreePtr(req.parser.Parse(analysisText));
    double parseMs = parseTimer.ElapsedMs();

    utils::HighResTimer colTimer;
    angel_lsp::analysis::SymbolTable staging;
    const bool contributes = PredefinedStubContributes(req.uriStr);
    if (contributes && tree)
    {
        m_symbolCollector->CollectSymbolsWithTree({req.uriStr, analysisText, m_i18n.get()}, tree.get(), staging);
    }
    double colMs = colTimer.ElapsedMs();

    StorePredefinedSnapshot(req, analysisText, tree, contributes);

    std::shared_ptr<angel_lsp::analysis::Scope> scopeRoot;
    std::vector<analysis::CallSite> calls;
    double scopeMs = 0.0;
    double checkMs = 0.0;

    const double totalMs = totalTimer.ElapsedMs();
    LogInfo(fmt::format("[Analysis] Finished predefined analysis for file: {} in {} (Parse: {}, Symbols: {})",
                        req.uriStr, utils::FormatDuration(totalMs), utils::FormatDuration(parseMs),
                        utils::FormatDuration(colMs)));
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {} (Parse: {}, Collector: {}"
                        ", Scopes: {}, Checkers: {})",
                        req.uriStr, utils::FormatDuration(totalMs), utils::FormatDuration(parseMs),
                        utils::FormatDuration(colMs), utils::FormatDuration(scopeMs), utils::FormatDuration(checkMs)));

    const bool committed = CommitAnalysisResults({
        .uriStr = req.uriStr,
        .version = req.version,
        .generation = req.generation,
        .configRevision = req.configRevision,
        .staging = &staging,
        .scopeRoot = std::move(scopeRoot),
        .calls = std::move(calls),
        .diagnostics = {},
        .text = req.text,
        .closedRevision = req.closedRevision,
    });

    if (!committed)
    {
        return;
    }

    const bool wordsChanged = RefreshStubDefinedWords(req.uriStr, req.text);
    if (wordsChanged)
    {
        ReanalyseOpenDocuments();
    }
}

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
    if (ts_node_has_error(syntax->index.Root()))
    {
        m_symbolTable.RetainLastKnownGoodTypes(req.uriStr, staging);
    }
    return {std::move(syntax), std::move(diagnostics), parseMs, (colTimer.ElapsedMs() - parseMs), extractedNodes};
}

void Server::LogAnalysisProfile(std::string_view prefix, const std::string& uriStr,
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

const analysis::SymbolTable* Server::PrepareAnalysisSnapshot(const std::string& uriStr,
                                                             const analysis::SymbolTable* staging,
                                                             std::unique_ptr<analysis::SymbolTable>& outSnapshot) const
{
    const auto moduleUris = ComputeModuleClosureUris(uriStr);
    const bool sameInterface =
        !staging || m_symbolTable.ComputeDocumentInterfaceHash(uriStr) == staging->ComputeDocumentInterfaceHash(uriStr);
    if (sameInterface && (moduleUris.empty() || m_config.modules.empty() ||
                          m_symbolTable.ContainsOnlyFiles(moduleUris, m_config.info.predefinedFileExtension)))
    {
        return &m_symbolTable;
    }
    const auto predefinedBase = GetWorkspaceStore().GetPredefinedBaseTable();
    outSnapshot = m_symbolTable.CreateAnalysisSnapshot({
        .uriStr = uriStr,
        .staging = staging,
        .allowedModuleFileUris = &moduleUris,
        .predefinedExtension = m_config.info.predefinedFileExtension,
        .predefinedBase = predefinedBase.get(),
    });
    return outSnapshot.get();
}

static void LogAnalysisProfileAndClosure(const Server& server, const std::string& uriStr, double closureMs,
                                         const AnalysisTimingProfile& profile)
{
    server.LogAnalysisProfile("[Analysis]", uriStr, profile);
    if (closureMs > 0.05)
    {
        server.LogInfo(
            fmt::format("[Analysis Breakdown] File: {} | Closure: {}", uriStr, utils::FormatDuration(closureMs)));
    }
}

std::vector<analysis::Diagnostic> Server::RunDocumentSemantics(angel_lsp::analysis::SemanticAnalysisRequest baseReq,
                                                               double& outCheckMs, bool fromScheduler)
{
    utils::HighResTimer checkTimer;
    if (fromScheduler && m_clearWorkerMixinCache.exchange(false, std::memory_order_acq_rel))
    {
        m_workerMixinCache.clear();
    }
    config::DiagnosticsConfig diagnosticConfig;
    config::EngineProperties engineProperties;
    {
        std::lock_guard lock(m_lifecycleMutex);
        diagnosticConfig = m_config.diagnostics;
        engineProperties = m_config.engine;
    }
    baseReq.diagnostics = &diagnosticConfig;
    baseReq.engineProperties = &engineProperties;

    analysis::rules::MixinFileCache inlineCache;
    baseReq.mixinCache = fromScheduler ? &m_workerMixinCache : &inlineCache;
    auto diagnostics = m_semanticAnalyzer->Analyze(baseReq);
    outCheckMs = checkTimer.ElapsedMs();
    return diagnostics;
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
    const TSNode root = index.Root();
    std::shared_ptr<analysis::Scope> scopes;
    if (const auto& extracted = parsed.closed.lexicalScopes)
    {
        scopes = analysis::CopyLexicalScopeTree(*extracted);
        LogInfo(fmt::format("[Analysis] Reused closed lexical scopes for file: {}", req.uriStr));
    }
    else if (m_localScopeCollector)
    {
        scopes = m_localScopeCollector->CollectScopesFromTree(root, req.text, &index);
    }

    std::vector<analysis::CallSite> calls;
    if (!parsed.closed.indexedSyntax)
    {
        calls = scopes ? analysis::CollectCallsFromScopeTree(*scopes) : analysis::CollectCalls(root, req.text);
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
