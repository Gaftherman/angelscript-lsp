#include "analysis/CallGraph.h"
#include "analysis/NodeIndex.h"
#include "lsp/Server.h"
#include "utils/Constants.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
namespace
{
constexpr std::chrono::milliseconds k_analysisDebounce{200};
}

void Server::ReanalyseOpenDocuments()
{
    const auto docs = m_documentStore.GetAllDocuments();
    for (const auto& doc : docs)
    {
        if (!doc || angel_lsp::utils::IsPredefinedFile(doc->uri, m_config.info.predefinedFileExtension))
        {
            continue;
        }
        if (m_analysisScheduler && m_analysisScheduler->ShouldDebouncePeer(doc->uri))
        {
            continue;
        }
        IndexModuleClosure(doc->uri);
        SharedTree astSnapshot = doc->getAST();
        document::TreePtr treeCopy = document::MakeTreePtr(astSnapshot ? ts_tree_copy(astSnapshot.get()) : nullptr);
        ScheduleAnalysis(ScheduleAnalysisRequest{.uriStr = doc->uri,
                                                 .text = doc->text,
                                                 .force = false,
                                                 .tree = std::move(treeCopy),
                                                 .version = doc->version,
                                                 .generation = doc->generation});
    }
}

angel_lsp::analysis::SemanticAnalysisRequest
Server::BuildAnalysisRequest(const std::string& uriStr, const std::string& text, const TSTree* tree,
                             const angel_lsp::analysis::SymbolTable* customSymbolTable) const
{
    angel_lsp::analysis::SemanticAnalysisRequest request{customSymbolTable ? *customSymbolTable : m_symbolTable, uriStr,
                                                         std::string(m_config.info.predefinedFileExtension),
                                                         m_i18n.get()};

    request.typeConfig = &m_config.types;
    request.engineProperties = &m_config.engine;

    if (const std::string ownPath = CanonicalPathFromUri(uriStr); !ownPath.empty())
    {
        request.moduleFileUris.insert(uriStr);

        for (const auto& path : m_includeGraph.GetModuleClosure(ownPath))
        {
            if (path == ownPath)
                continue;

            const auto indexed = m_indexedUriByPath.find(path);
            request.moduleFileUris.insert(indexed != m_indexedUriByPath.end() ? indexed->second : UriFromPath(path));
        }
    }
    request.moduleContext = ModuleContextFor(uriStr);

    request.diagnostics = &m_config.diagnostics;
    request.severityOverrides = m_diagnosticSeverities.empty() ? nullptr : &m_diagnosticSeverities;
    request.enableTypeConversionChecks = m_config.features.enableTypeConversionChecks;
    request.scopeRoot = m_scopeIndex.GetRoot(uriStr);
    request.sourceCode = text;
    request.tree = tree;

    auto scan = angel_lsp::utils::ScanPreprocessor(text, *DefinedWords(), m_config.preprocessor,
                                                   m_config.pragmaMode != config::ServerConfig::PragmaMode::Accept);

    request.excludedLineRanges = std::move(scan.excluded);

    if (!angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        request.unsupportedDirectives = std::move(scan.unsupported);

    request.pragmaSeverity = m_config.pragmaMode == config::ServerConfig::PragmaMode::Error
                                 ? angel_lsp::analysis::DiagnosticSeverity::Error
                                 : angel_lsp::analysis::DiagnosticSeverity::Hint;

    return request;
}

std::vector<angel_lsp::analysis::Diagnostic> Server::ReplaceSymbolsFromTree(const std::string& uriStr,
                                                                            const std::string& text, TSTree* tree,
                                                                            bool* outInterfaceChanged)
{
    angel_lsp::analysis::SymbolTable staging;
    auto diagnostics = m_symbolCollector->CollectSymbolsWithTree({uriStr, text, m_i18n.get()}, tree, staging);
    if (outInterfaceChanged)
    {
        const uint64_t oldHash = m_symbolTable.ComputeDocumentInterfaceHash(uriStr);
        const uint64_t newHash = staging.ComputeDocumentInterfaceHash(uriStr);
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
    if (request.text.size() > angel_lsp::constants::limits::MaxAnalysedDocumentBytes)
    {
        LogWarning(fmt::format("Skipping analysis of {}: {} bytes exceeds the {} byte limit. Navigation still works.",
                               request.uriStr, request.text.size(),
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
        const TSNode root = ts_tree_root_node(request.tree);
        scopeRoot = m_localScopeCollector->CollectScopesFromTree(root, request.text);
        m_callGraph.SetDocumentCalls(request.uriStr, analysis::CollectCalls(root, request.text));
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

    std::unique_ptr<analysis::NodeIndex> localNodeIndex;
    const analysis::NodeIndex* nodeIndex = request.nodeIndex;
    if (!nodeIndex && request.tree)
    {
        localNodeIndex = std::make_unique<analysis::NodeIndex>(ts_tree_root_node(request.tree));
        nodeIndex = localNodeIndex.get();
    }

    auto analysisReq = BuildAnalysisRequest(request.uriStr, request.text, request.tree);
    analysisReq.scopeRoot = scopeRoot;
    analysisReq.mutableScopeRoot = scopeRoot.get();
    analysisReq.nodeIndex = nodeIndex;

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

void Server::ScheduleAnalysis(ScheduleAnalysisRequest req)
{
    if (req.version < 0)
    {
        req.version = GetDocumentVersion(req.uriStr);
    }

    if (req.generation == 0)
    {
        req.generation = m_documentStore.GetGeneration(req.uriStr);
    }

    const uint64_t configRevision = m_configRevision.load();
    const std::string analysisText = AnalysisTextFor(req.uriStr, req.text);

    if (m_analysisScheduler)
    {
        m_analysisScheduler->Schedule(
            {req.uriStr, analysisText, req.force, std::move(req.tree), req.version, req.generation, configRevision});
    }
}

void Server::ScheduleAnalysis(const std::string& uriStr, const std::string& text, bool force)
{
    ScheduleAnalysis(ScheduleAnalysisRequest{.uriStr = uriStr, .text = text, .force = force});
}

void Server::ScheduleAnalysisImmediate(ScheduleAnalysisRequest req)
{
    if (req.version < 0)
    {
        req.version = GetDocumentVersion(req.uriStr);
    }

    if (req.generation == 0)
    {
        req.generation = m_documentStore.GetGeneration(req.uriStr);
    }

    const uint64_t configRevision = m_configRevision.load();
    const std::string analysisText = AnalysisTextFor(req.uriStr, req.text);

    if (m_analysisScheduler)
    {
        m_analysisScheduler->ScheduleImmediate(
            {req.uriStr, analysisText, req.force, std::move(req.tree), req.version, req.generation, configRevision});
    }
}

void Server::ScheduleAnalysisImmediate(const std::string& uriStr, const std::string& text, bool force)
{
    ScheduleAnalysisImmediate(ScheduleAnalysisRequest{.uriStr = uriStr, .text = text, .force = force});
}

bool Server::IsCommitAnalysisStale(const CommitAnalysisRequest& req) const
{
    if (req.generation > 0 && !m_documentStore.IsCurrent(req.uriStr, req.generation, req.version))
    {
        return true;
    }
    if (req.configRevision != m_configRevision.load())
    {
        return true;
    }
    if (m_analysisScheduler && m_analysisScheduler->IsCancelled(req.uriStr))
    {
        return true;
    }

    const int currentVer = GetDocumentVersion(req.uriStr);
    if (currentVer >= 0 && req.version >= 0 && req.version != currentVer)
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

    if (IsCommitAnalysisStale(req))
    {
        return false;
    }

    if (req.staging)
    {
        m_symbolTable.ReplaceDocumentSymbols(req.uriStr, std::move(*req.staging));
    }

    if (req.scopeRoot)
    {
        m_scopeIndex.SetScopeTree(req.uriStr, std::move(req.scopeRoot));
    }
    else
    {
        m_scopeIndex.ClearDocument(req.uriStr);
    }
    m_callGraph.SetDocumentCalls(req.uriStr, std::move(req.calls));

    PublishDiagnostics(PublishDiagnosticsRequest{
        .uriStr = req.uriStr,
        .text = req.text,
        .diagnostics = std::move(req.diagnostics),
        .version = req.version,
        .generation = req.generation,
    });
    return true;
}

bool Server::IsAnalyzeDocumentStale(const AnalyzeDocumentRequest& req) const
{
    if (req.generation > 0 && !m_documentStore.IsCurrent(req.uriStr, req.generation, req.version))
    {
        return true;
    }
    if (m_analysisScheduler && m_analysisScheduler->IsCancelled(req.uriStr))
    {
        return true;
    }

    const int currentVersion = GetDocumentVersion(req.uriStr);
    if (currentVersion >= 0 && req.version >= 0 && req.version != currentVersion)
    {
        return true;
    }
    return false;
}

void Server::AnalyzePredefinedDocument(AnalyzeDocumentRequest req, const utils::HighResTimer& totalTimer)
{
    const std::string analysisText = AnalysisTextFor(req.uriStr, req.text);
    utils::HighResTimer parseTimer;
    document::TreePtr tree =
        req.treeCopy ? std::move(req.treeCopy) : document::MakeTreePtr(req.parser.Parse(analysisText));
    double parseMs = parseTimer.ElapsedMs();

    utils::HighResTimer colTimer;
    angel_lsp::analysis::SymbolTable staging;
    const bool contributes = PredefinedStubContributes(req.uriStr);
    if (contributes && tree)
    {
        m_symbolCollector->CollectSymbolsWithTree({req.uriStr, analysisText, m_i18n.get()}, tree.get(), staging);
    }
    double colMs = colTimer.ElapsedMs();

    utils::HighResTimer scopeTimer;
    std::shared_ptr<angel_lsp::analysis::Scope> scopeRoot;
    std::vector<analysis::CallSite> calls;
    if (tree)
    {
        const TSNode root = ts_tree_root_node(tree.get());
        scopeRoot = m_localScopeCollector->CollectScopesFromTree(root, analysisText);
        calls = analysis::CollectCalls(root, analysisText);
    }
    double scopeMs = scopeTimer.ElapsedMs();
    double checkMs = 0.0;

    if (contributes)
    {
        ClaimPredefinedFile(req.uriStr, true);
        m_predefinedManager.SetDocumentText(req.uriStr, analysisText);
    }
    else
    {
        m_predefinedManager.RemoveStub(req.uriStr);
    }

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

    double totalMs = totalTimer.ElapsedMs();
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: {:.2f} "
                        "ms, Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
                        req.uriStr, totalMs, parseMs, colMs, scopeMs, checkMs));
}

void Server::AnalyzeNormalDocument(AnalyzeDocumentRequest req, const utils::HighResTimer& totalTimer)
{
    IndexModuleClosure(req.uriStr);

    utils::HighResTimer parseTimer;
    document::TreePtr tree = req.treeCopy ? std::move(req.treeCopy) : document::MakeTreePtr(req.parser.Parse(req.text));
    double parseMs = parseTimer.ElapsedMs();
    if (!tree)
    {
        return;
    }

    const TSNode root = ts_tree_root_node(tree.get());
    analysis::NodeIndex nodeIndex(root);

    utils::HighResTimer colTimer;
    angel_lsp::analysis::SymbolTable staging;
    auto diagnostics =
        m_symbolCollector->CollectSymbolsWithTree({req.uriStr, req.text, m_i18n.get()}, tree.get(), staging);
    double colMs = colTimer.ElapsedMs();

    if (req.generation > 0 && !m_documentStore.IsCurrent(req.uriStr, req.generation, req.version))
    {
        return;
    }
    if (m_analysisScheduler && m_analysisScheduler->IsCancelled(req.uriStr))
    {
        return;
    }

    std::unique_ptr<analysis::SymbolTable> analysisSnapshot = m_symbolTable.CreateAnalysisSnapshot(req.uriStr, staging);

    utils::HighResTimer scopeTimer;
    std::shared_ptr<angel_lsp::analysis::Scope> scopeRoot =
        m_localScopeCollector->CollectScopesFromTree(root, req.text);
    std::vector<analysis::CallSite> calls = analysis::CollectCalls(root, req.text);
    double scopeMs = scopeTimer.ElapsedMs();

    utils::HighResTimer checkTimer;
    auto request = BuildAnalysisRequest(req.uriStr, req.text, tree.get(), analysisSnapshot.get());
    request.scopeRoot = scopeRoot;
    request.mutableScopeRoot = scopeRoot.get();
    request.nodeIndex = &nodeIndex;

    auto semanticDiagnostics = m_semanticAnalyzer->Analyze(request);
    double checkMs = checkTimer.ElapsedMs();

    diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());
    AppendIncludeDiagnostics(req.uriStr, req.text, diagnostics);

    CommitAnalysisResults({
        .uriStr = req.uriStr,
        .version = req.version,
        .generation = req.generation,
        .configRevision = req.configRevision,
        .staging = &staging,
        .scopeRoot = std::move(scopeRoot),
        .calls = std::move(calls),
        .diagnostics = std::move(diagnostics),
        .text = req.text,
    });

    double totalMs = totalTimer.ElapsedMs();
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: {:.2f} ms, "
                        "Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
                        req.uriStr, totalMs, parseMs, colMs, scopeMs, checkMs));
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
        AnalyzeNormalDocument(std::move(req), totalTimer);
    }
}
} // namespace angel_lsp
