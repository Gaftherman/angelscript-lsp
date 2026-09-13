#include "lsp/Server.h"
#include "utils/Utils.h"
#include "utils/Timer.h"
#include "utils/PreprocessorRegions.h"
#include "analysis/CallGraph.h"
#include "analysis/NodeIndex.h"
#include "utils/Constants.h"
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
    namespace
    {
        constexpr std::chrono::milliseconds k_analysisDebounce{ 200 };
    }

    void Server::ReanalyseOpenDocuments()
    {
        const auto docs = m_documentStore.GetAllDocuments();
        for (const auto &doc : docs)
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
            document::TreePtr treeCopy = document::MakeTreePtr(doc->tree ? ts_tree_copy(doc->tree.get()) : nullptr);
            ScheduleAnalysis(doc->uri, doc->text, /*force=*/false, std::move(treeCopy), doc->version, doc->generation);
        }
    }

    angel_lsp::analysis::SemanticAnalysisRequest Server::BuildAnalysisRequest(const std::string &uriStr,
                                                                              const std::string &text,
                                                                              const TSTree *tree,
                                                                              const angel_lsp::analysis::SymbolTable *customSymbolTable) const
    {
        angel_lsp::analysis::SemanticAnalysisRequest request{
            customSymbolTable ? *customSymbolTable : m_symbolTable, uriStr, std::string(m_config.info.predefinedFileExtension), m_i18n.get() };

        request.typeConfig = &m_config.types;
        request.engineProperties = &m_config.engine;

        if (const std::string ownPath = CanonicalPathFromUri(uriStr); !ownPath.empty())
        {
            request.moduleFileUris.insert(uriStr);

            for (const auto &path : m_includeGraph.GetModuleClosure(ownPath))
            {
                if (path == ownPath)
                    continue;

                const auto indexed = m_indexedUriByPath.find(path);
                request.moduleFileUris.insert(indexed != m_indexedUriByPath.end() ? indexed->second
                                                                                 : UriFromPath(path));
            }
        }
        request.moduleContext = ModuleContextFor(uriStr);

        request.diagnostics = &m_config.diagnostics;
        request.severityOverrides = m_diagnosticSeverities.empty() ? nullptr : &m_diagnosticSeverities;
        request.enableTypeConversionChecks = m_config.features.enableTypeConversionChecks;
        request.scopeRoot = m_scopeIndex.GetRoot(uriStr);
        request.sourceCode = text;
        request.tree = tree;

        auto scan = angel_lsp::utils::ScanPreprocessor(
            text, *DefinedWords(), m_config.preprocessor,
            m_config.pragmaMode != config::ServerConfig::PragmaMode::Accept);

        request.excludedLineRanges = std::move(scan.excluded);

        if (!angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
            request.unsupportedDirectives = std::move(scan.unsupported);

        request.pragmaSeverity = m_config.pragmaMode == config::ServerConfig::PragmaMode::Error
                                     ? angel_lsp::analysis::DiagnosticSeverity::Error
                                     : angel_lsp::analysis::DiagnosticSeverity::Hint;

        return request;
    }

    std::vector<angel_lsp::analysis::Diagnostic> Server::ReplaceSymbolsFromTree(const std::string &uriStr,
                                                                                 const std::string &text,
                                                                                 TSTree *tree,
                                                                                 bool *outInterfaceChanged)
    {
        angel_lsp::analysis::SymbolTable staging;
        auto diagnostics = m_symbolCollector->CollectSymbolsWithTree(uriStr, text, tree, staging, m_i18n.get(), &m_config.types);
        if (outInterfaceChanged)
        {
            const uint64_t oldHash = m_symbolTable.ComputeDocumentInterfaceHash(uriStr);
            const uint64_t newHash = staging.ComputeDocumentInterfaceHash(uriStr);
            *outInterfaceChanged = (oldHash != newHash);
        }
        m_symbolTable.ReplaceDocumentSymbols(uriStr, std::move(staging));
        return diagnostics;
    }

    std::vector<angel_lsp::analysis::Diagnostic> Server::ReplaceSymbolsFromSource(const std::string &uriStr,
                                                                                   const std::string &text,
                                                                                   angel_lsp::parser::AngelScriptParser &parser)
    {
        angel_lsp::analysis::SymbolTable staging;
        auto diagnostics = m_symbolCollector->CollectSymbols(uriStr, text, parser, staging, m_i18n.get(), &m_config.types);
        m_symbolTable.ReplaceDocumentSymbols(uriStr, std::move(staging));
        return diagnostics;
    }

    std::vector<angel_lsp::analysis::Diagnostic> Server::CollectScopesAndAnalyze(const std::string &uriStr,
                                                                                 const std::string &text,
                                                                                 const TSTree *tree,
                                                                                 double *outScopeMs,
                                                                                 double *outCheckMs,
                                                                                 const analysis::NodeIndex *nodeIndex)
    {
        if (text.size() > angel_lsp::constants::limits::MaxAnalysedDocumentBytes)
        {
            LogWarning(fmt::format(
                "Skipping analysis of {}: {} bytes exceeds the {} byte limit. Navigation still works.",
                uriStr, text.size(), angel_lsp::constants::limits::MaxAnalysedDocumentBytes));

            m_scopeIndex.ClearDocument(uriStr);
            m_callGraph.ClearDocument(uriStr);
            if (outScopeMs)
            {
                *outScopeMs = 0.0;
            }
            if (outCheckMs)
            {
                *outCheckMs = 0.0;
            }
            return {};
        }

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            if (outScopeMs)
            {
                *outScopeMs = 0.0;
            }
            if (outCheckMs)
            {
                *outCheckMs = 0.0;
            }
            return {};
        }

        std::shared_ptr<angel_lsp::analysis::Scope> scopeRoot;

        utils::HighResTimer scopeTimer;
        if (tree)
        {
            const TSNode root = ts_tree_root_node(tree);
            scopeRoot = m_localScopeCollector->CollectScopesFromTree(root, text);
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(root, text));
        }
        else
        {
            m_scopeIndex.ClearDocument(uriStr);
            m_callGraph.ClearDocument(uriStr);
        }
        if (outScopeMs)
        {
            *outScopeMs = scopeTimer.ElapsedMs();
        }

        std::unique_ptr<analysis::NodeIndex> localNodeIndex;
        if (!nodeIndex && tree)
        {
            localNodeIndex = std::make_unique<analysis::NodeIndex>(ts_tree_root_node(tree));
            nodeIndex = localNodeIndex.get();
        }

        auto request = BuildAnalysisRequest(uriStr, text, tree);

        request.scopeRoot = scopeRoot;
        request.mutableScopeRoot = scopeRoot.get();
        request.nodeIndex = nodeIndex;

        utils::HighResTimer checkTimer;
        auto diagnostics = m_semanticAnalyzer->Analyze(request);
        if (outCheckMs)
        {
            *outCheckMs = checkTimer.ElapsedMs();
        }

        if (scopeRoot)
        {
            m_scopeIndex.SetScopeTree(uriStr, std::shared_ptr<const angel_lsp::analysis::Scope>(std::move(scopeRoot)));
        }

        return diagnostics;
    }

    void Server::ScheduleAnalysis(const std::string &uriStr, const std::string &text, bool force, angel_lsp::document::TreePtr tree, int version, uint64_t generation)
    {
        if (version < 0)
        {
            version = GetDocumentVersion(uriStr);
        }

        if (generation == 0)
        {
            generation = m_documentStore.GetGeneration(uriStr);
        }

        const uint64_t configRevision = m_configRevision.load();
        const std::string analysisText = AnalysisTextFor(uriStr, text);

        if (m_analysisScheduler)
        {
            m_analysisScheduler->Schedule(uriStr, analysisText, force, std::move(tree), version, generation, configRevision);
        }
    }

    void Server::ScheduleAnalysisImmediate(const std::string &uriStr, const std::string &text, bool force, angel_lsp::document::TreePtr tree, int version, uint64_t generation)
    {
        if (version < 0)
        {
            version = GetDocumentVersion(uriStr);
        }

        if (generation == 0)
        {
            generation = m_documentStore.GetGeneration(uriStr);
        }

        const uint64_t configRevision = m_configRevision.load();
        const std::string analysisText = AnalysisTextFor(uriStr, text);

        if (m_analysisScheduler)
        {
            m_analysisScheduler->ScheduleImmediate(uriStr, analysisText, force, std::move(tree), version, generation, configRevision);
        }
    }

    bool Server::CommitAnalysisResults(const std::string &uriStr,
                                       int version,
                                       uint64_t generation,
                                       uint64_t configRevision,
                                       analysis::SymbolTable &&staging,
                                       std::shared_ptr<const analysis::Scope> scopeRoot,
                                       std::vector<analysis::CallSite> calls,
                                       std::vector<analysis::Diagnostic> diagnostics,
                                       const std::string &text)
    {
        if (m_onBeforeCommitHook)
        {
            m_onBeforeCommitHook(uriStr, version, generation);
        }

        std::lock_guard<std::mutex> lock(m_lifecycleMutex);

        if (generation > 0 && !m_documentStore.IsCurrent(uriStr, generation, version))
        {
            return false;
        }
        if (configRevision != m_configRevision.load())
        {
            return false;
        }
        if (m_analysisScheduler && m_analysisScheduler->IsCancelled(uriStr))
        {
            return false;
        }

        const int currentVer = GetDocumentVersion(uriStr);
        if (currentVer >= 0 && version >= 0 && version != currentVer)
        {
            return false;
        }

        // Commit isolated results atomically
        m_symbolTable.ReplaceDocumentSymbols(uriStr, std::move(staging));

        if (scopeRoot)
        {
            m_scopeIndex.SetScopeTree(uriStr, std::shared_ptr<const angel_lsp::analysis::Scope>(std::move(scopeRoot)));
        }
        else
        {
            m_scopeIndex.ClearDocument(uriStr);
        }
        m_callGraph.SetDocumentCalls(uriStr, std::move(calls));

        PublishDiagnostics(uriStr, text, diagnostics, version, generation);
        return true;
    }

    void Server::AnalyzeDocument(const std::string &uriStr, const std::string &text,
                                 angel_lsp::parser::AngelScriptParser &parser,
                                 angel_lsp::document::TreePtr treeCopy, int version, uint64_t generation, uint64_t configRevision)
    {
        utils::HighResTimer totalTimer;

        if (generation > 0 && !m_documentStore.IsCurrent(uriStr, generation, version))
        {
            return;
        }
        if (m_analysisScheduler && m_analysisScheduler->IsCancelled(uriStr))
        {
            return;
        }

        const int currentVersion = GetDocumentVersion(uriStr);
        if (currentVersion >= 0 && version >= 0 && version != currentVersion)
        {
            return;
        }

        const bool isPredefined = angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension);
        if (isPredefined)
        {
            const std::string analysisText = AnalysisTextFor(uriStr, text);
            utils::HighResTimer parseTimer;
            document::TreePtr tree = treeCopy ? std::move(treeCopy) : document::MakeTreePtr(parser.Parse(analysisText));
            double parseMs = parseTimer.ElapsedMs();

            utils::HighResTimer colTimer;
            angel_lsp::analysis::SymbolTable staging;
            const bool contributes = PredefinedStubContributes(uriStr);
            if (contributes && tree)
            {
                m_symbolCollector->CollectSymbolsWithTree(uriStr, analysisText, tree.get(), staging, m_i18n.get(), &m_config.types);
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
                ClaimPredefinedFile(uriStr, /*forceReload=*/true);
                m_predefinedManager.SetDocumentText(uriStr, analysisText);
            }

            const bool committed = CommitAnalysisResults(
                uriStr, version, generation, configRevision,
                std::move(staging), std::move(scopeRoot), std::move(calls),
                {}, text);

            if (!committed)
            {
                return;
            }

            const bool wordsChanged = RefreshStubDefinedWords(uriStr, text);
            if (wordsChanged)
            {
                ReanalyseOpenDocuments();
            }

            double totalMs = totalTimer.ElapsedMs();
            LogInfo(fmt::format(
                "[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: {:.2f} ms, Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
                uriStr, totalMs, parseMs, colMs, scopeMs, checkMs));

            return;
        }

        IndexModuleClosure(uriStr);

        utils::HighResTimer parseTimer;
        document::TreePtr tree = treeCopy ? std::move(treeCopy) : document::MakeTreePtr(parser.Parse(text));
        double parseMs = parseTimer.ElapsedMs();
        if (!tree)
        {
            return;
        }

        const TSNode root = ts_tree_root_node(tree.get());
        analysis::NodeIndex nodeIndex(root);

        utils::HighResTimer colTimer;
        angel_lsp::analysis::SymbolTable staging;
        auto diagnostics = m_symbolCollector->CollectSymbolsWithTree(uriStr, text, tree.get(), staging, m_i18n.get(), &m_config.types);
        double colMs = colTimer.ElapsedMs();

        // Currency check before snapshot creation
        if (generation > 0 && !m_documentStore.IsCurrent(uriStr, generation, version))
        {
            return;
        }
        if (m_analysisScheduler && m_analysisScheduler->IsCancelled(uriStr))
        {
            return;
        }

        // Create isolated analysis snapshot combining m_symbolTable and staging without polluting m_symbolTable
        std::unique_ptr<analysis::SymbolTable> analysisSnapshot = m_symbolTable.CreateAnalysisSnapshot(uriStr, staging);

        utils::HighResTimer scopeTimer;
        std::shared_ptr<angel_lsp::analysis::Scope> scopeRoot = m_localScopeCollector->CollectScopesFromTree(root, text);
        std::vector<analysis::CallSite> calls = analysis::CollectCalls(root, text);
        double scopeMs = scopeTimer.ElapsedMs();

        utils::HighResTimer checkTimer;
        auto request = BuildAnalysisRequest(uriStr, text, tree.get(), analysisSnapshot.get());
        request.scopeRoot = scopeRoot;
        request.mutableScopeRoot = scopeRoot.get();
        request.nodeIndex = &nodeIndex;

        auto semanticDiagnostics = m_semanticAnalyzer->Analyze(request);
        double checkMs = checkTimer.ElapsedMs();

        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());
        AppendIncludeDiagnostics(uriStr, text, diagnostics);

        // Atomic commit point: validates currency under lifecycle lock and publishes results
        CommitAnalysisResults(
            uriStr, version, generation, configRevision,
            std::move(staging), std::move(scopeRoot), std::move(calls),
            std::move(diagnostics), text);

        double totalMs = totalTimer.ElapsedMs();
        LogInfo(fmt::format(
            "[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: {:.2f} ms, Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
            uriStr, totalMs, parseMs, colMs, scopeMs, checkMs));
    }
}
