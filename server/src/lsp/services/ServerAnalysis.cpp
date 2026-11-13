#include "analysis/index/CallGraph.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "lsp/Server.h"
#include "parser/SyntaxTree.h"
#include "utils/Constants.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <algorithm>
#include <spdlog/fmt/fmt.h>
#include <string_view>
#include <vector>

namespace angel_lsp
{
ankerl::unordered_dense::set<std::string> Server::ComputeModuleClosureUris(std::string_view uriStr) const
{
    ankerl::unordered_dense::set<std::string> moduleFileUris;
    moduleFileUris.insert(std::string(uriStr));
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
Server::BuildAnalysisRequest(std::string_view uriStr, std::string_view text, const analysis::NodeIndex* nodeIndex,
                             const utils::PreprocessorScan& preprocessing,
                             const angel_lsp::analysis::SymbolTable* customSymbolTable) const
{
    angel_lsp::analysis::SemanticAnalysisRequest request{customSymbolTable ? *customSymbolTable : m_symbolTable, std::string(uriStr),
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
    request.scopeRoot = m_scopeIndex.GetRoot(std::string(uriStr));
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

std::vector<angel_lsp::analysis::Diagnostic>
Server::ReplaceSymbolsFromTree(std::string_view uriStr, std::string_view text, const parser::TreePtr* tree,
                               bool* outInterfaceChanged, const analysis::NodeIndex* index)
{
    angel_lsp::analysis::SymbolTable staging;
    const analysis::SymbolCollectRequest request{std::string(uriStr), text, m_i18n.get()};
    auto diagnostics = index
                           ? m_symbolCollector->CollectSymbolsWithIndex(request, *index, staging)
                           : m_symbolCollector->CollectSymbolsWithTree(request, tree ? tree->get() : nullptr, staging);
    if ((index && index->HasSyntaxErrors()) || (tree && parser::TreeHasError(*tree)))
    {
        m_symbolTable.RetainLastKnownGoodTypes(std::string(uriStr), staging);
    }
    if (outInterfaceChanged)
    {
        const uint64_t newHash = staging.ComputeDocumentInterfaceHash(std::string(uriStr));
        uint64_t oldHash = m_symbolTable.ComputeDocumentInterfaceHash(std::string(uriStr));
        {
            std::lock_guard<std::mutex> lock(m_closureMutex);
            if (const auto it = m_lastSavedInterfaceHash.find(std::string(uriStr)); it != m_lastSavedInterfaceHash.end())
            {
                oldHash = it->second;
            }
            m_lastSavedInterfaceHash[std::string(uriStr)] = newHash;
        }

        *outInterfaceChanged = (oldHash != newHash);
    }
    m_symbolTable.ReplaceDocumentSymbols(std::string(uriStr), std::move(staging));
    return diagnostics;
}

std::vector<angel_lsp::analysis::Diagnostic>
Server::ReplaceSymbolsFromSource(std::string_view uriStr, std::string_view text,
                                 angel_lsp::parser::AngelScriptParser& parser)
{
    angel_lsp::analysis::SymbolTable staging;
    auto diagnostics = m_symbolCollector->CollectSymbols({std::string(uriStr), text, m_i18n.get()}, parser, staging);
    m_symbolTable.ReplaceDocumentSymbols(std::string(uriStr), std::move(staging));
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
    if (request.hasTree)
    {
        scopeRoot = m_localScopeCollector->CollectScopesFromIndex(request.nodeIndex, request.source.text);
        if (scopeRoot)
        {
            m_callGraph.SetDocumentCalls(request.uriStr, analysis::CollectCallsFromScopeTree(*scopeRoot));
        }
        else
        {
            m_callGraph.SetDocumentCalls(request.uriStr,
                                         analysis::CollectCallsFromIndex(request.nodeIndex, request.source.text));
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
} // namespace angel_lsp
