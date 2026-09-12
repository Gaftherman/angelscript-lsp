#include "lsp/Server.h"
#include "utils/Utils.h"
#include "utils/Timer.h"
#include "utils/PreprocessorRegions.h"
#include "analysis/CallGraph.h"
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
        const auto now = std::chrono::steady_clock::now();
        for (const auto &[openUri, text] : m_openDocuments)
        {
            if (angel_lsp::utils::IsPredefinedFile(openUri, m_config.info.predefinedFileExtension))
            {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_peerDebounceMutex);
                auto it = m_peerAnalysisTimestamps.find(openUri);
                if (it != m_peerAnalysisTimestamps.end())
                {
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second) < k_peerAnalysisDebounceWindow)
                    {
                        continue;
                    }
                }
                m_peerAnalysisTimestamps[openUri] = now;
            }
            IndexModuleClosure(openUri);
            ScheduleAnalysis(openUri, text);
        }
    }

    angel_lsp::analysis::SemanticAnalysisRequest Server::BuildAnalysisRequest(const std::string &uriStr,
                                                                              const std::string &text,
                                                                              const TSTree *tree) const
    {
        angel_lsp::analysis::SemanticAnalysisRequest request{
            m_symbolTable, uriStr, std::string(m_config.info.predefinedFileExtension), m_i18n.get() };

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
                                                                                 double *outCheckMs)
    {
        if (text.size() > angel_lsp::constants::limits::MaxAnalysedDocumentBytes)
        {
            LogWarning(fmt::format(
                "Skipping analysis of {}: {} bytes exceeds the {} byte limit. Navigation still works.",
                uriStr, text.size(), angel_lsp::constants::limits::MaxAnalysedDocumentBytes));

            m_scopeIndex.ClearDocument(uriStr);
            m_callGraph.ClearDocument(uriStr);
            if (outScopeMs) *outScopeMs = 0.0;
            if (outCheckMs) *outCheckMs = 0.0;
            return {};
        }

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            if (outScopeMs) *outScopeMs = 0.0;
            if (outCheckMs) *outCheckMs = 0.0;
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

        auto request = BuildAnalysisRequest(uriStr, text, tree);

        request.scopeRoot = scopeRoot;
        request.mutableScopeRoot = scopeRoot.get();

        utils::HighResTimer checkTimer;
        auto diagnostics = m_semanticAnalyzer->Analyze(request);
        if (outCheckMs)
        {
            *outCheckMs = checkTimer.ElapsedMs();
        }

        if (scopeRoot)
            m_scopeIndex.SetScopeTree(uriStr, std::shared_ptr<const angel_lsp::analysis::Scope>(std::move(scopeRoot)));

        return diagnostics;
    }

    void Server::ScheduleAnalysis(const std::string &uriStr, const std::string &text, bool force, angel_lsp::document::TreePtr tree, int version)
    {
        if (version < 0)
        {
            version = GetDocumentVersion(uriStr);
        }

        const std::string analysisText = AnalysisTextFor(uriStr, text);

        {
            std::lock_guard<std::mutex> lock(m_analysisMutex);
            m_savedUris.erase(uriStr);

            if (!force && m_currentlyAnalyzingUri == uriStr &&
                m_currentlyAnalyzingVersion >= version && version >= 0 &&
                m_currentlyAnalyzingText == analysisText)
            {
                return;
            }

            if (const auto running = m_analysisInFlight.find(uriStr);
                !force && running != m_analysisInFlight.end() && running->second.version >= version && version >= 0 && running->second.text == analysisText)
            {
                return;
            }

            const auto it = m_pendingAnalysis.find(uriStr);
            if (it != m_pendingAnalysis.end())
            {
                if (!force && it->second.text == analysisText)
                {
                    return;
                }

                if (!force && it->second.version > version && version >= 0)
                {
                    return;
                }

                it->second = PendingAnalysisEntry{ analysisText, std::move(tree), version };
            }
            else
            {
                m_pendingAnalysis.emplace(uriStr, PendingAnalysisEntry{ analysisText, std::move(tree), version });
            }

            ++m_analysisRevision;
        }

        m_analysisCv.notify_one();
    }

    void Server::RunAnalysisLoop()
    {
        angel_lsp::parser::AngelScriptParser parser(m_logger.get());

        for (;;)
        {
            std::unique_lock<std::mutex> lock(m_analysisMutex);
            m_analysisCv.wait(lock, [this]() { return m_analysisStop || !m_pendingAnalysis.empty(); });

            if (m_analysisStop)
                return;

            for (;;)
            {
                const uint64_t seen = m_analysisRevision;
                const bool interrupted = m_analysisCv.wait_for(lock, k_analysisDebounce, [this, seen]()
                                                               { return m_analysisStop || m_analysisRevision != seen; });

                if (m_analysisStop)
                    return;

                if (!interrupted)
                    break;
            }

            m_analysisInFlight.swap(m_pendingAnalysis);
            lock.unlock();

            for (;;)
            {
                std::string currentUri;
                PendingAnalysisEntry currentEntry;
                {
                    std::lock_guard<std::mutex> workLock(m_analysisMutex);
                    if (m_analysisInFlight.empty())
                    {
                        break;
                    }
                    auto it = m_analysisInFlight.begin();
                    currentUri = it->first;
                    currentEntry = std::move(it->second);
                    m_analysisInFlight.erase(it);

                    if (m_savedUris.erase(currentUri) > 0)
                    {
                        continue;
                    }
                    if (const auto pendingIt = m_pendingAnalysis.find(currentUri);
                        pendingIt != m_pendingAnalysis.end() && pendingIt->second.version > currentEntry.version && currentEntry.version >= 0)
                    {
                        continue;
                    }
                    m_currentlyAnalyzingUri = currentUri;
                    m_currentlyAnalyzingText = currentEntry.text;
                    m_currentlyAnalyzingVersion = currentEntry.version;
                }

                AnalyzeDocument(currentUri, currentEntry.text, parser, std::move(currentEntry.tree), currentEntry.version);

                {
                    std::lock_guard<std::mutex> workLock(m_analysisMutex);
                    m_currentlyAnalyzingUri.clear();
                    m_currentlyAnalyzingText.clear();
                    m_currentlyAnalyzingVersion = -1;
                }
            }
        }
    }

    void Server::AnalyzeDocument(const std::string &uriStr, const std::string &text,
                                 angel_lsp::parser::AngelScriptParser &parser,
                                 angel_lsp::document::TreePtr treeCopy, int version)
    {
        utils::HighResTimer totalTimer;

        const int currentVersion = GetDocumentVersion(uriStr);
        if (currentVersion >= 0 && version >= 0 && version < currentVersion)
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
            {
                std::lock_guard<std::mutex> lock(m_predefinedMutex);
                if (PredefinedStubContributes(uriStr))
                {
                    ClaimPredefinedFile(uriStr, /*forceReload=*/true);
                    m_predefinedDocuments[uriStr] = analysisText;
                    ReplaceSymbolsFromTree(uriStr, analysisText, tree.get());
                }
            }
            double colMs = colTimer.ElapsedMs();

            utils::HighResTimer scopeTimer;
            m_scopeIndex.ClearDocument(uriStr);
            m_callGraph.ClearDocument(uriStr);
            if (tree)
            {
                m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(tree.get()), analysisText));
                m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(tree.get()), analysisText));
            }
            double scopeMs = scopeTimer.ElapsedMs();
            double checkMs = 0.0;

            PublishDiagnostics(uriStr, text, {}, version);

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

        utils::HighResTimer colTimer;
        angel_lsp::analysis::SymbolTable staging;
        auto diagnostics = m_symbolCollector->CollectSymbolsWithTree(uriStr, text, tree.get(), staging, m_i18n.get(), &m_config.types);
        m_symbolTable.ReplaceDocumentSymbols(uriStr, std::move(staging));
        double colMs = colTimer.ElapsedMs();

        double scopeMs = 0.0;
        double checkMs = 0.0;
        auto semanticDiagnostics = CollectScopesAndAnalyze(uriStr, text, tree.get(), &scopeMs, &checkMs);
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

        AppendIncludeDiagnostics(uriStr, text, diagnostics);

        PublishDiagnostics(uriStr, text, diagnostics, version);

        double totalMs = totalTimer.ElapsedMs();
        LogInfo(fmt::format(
            "[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: {:.2f} ms, Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
            uriStr, totalMs, parseMs, colMs, scopeMs, checkMs));
    }
}
