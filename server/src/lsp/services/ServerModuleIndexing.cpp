#include "analysis/index/CallGraph.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/RuleIndex.h"
#include "lsp/Server.h"
#include "lsp/services/ServerModuleUtils.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp
{
void Server::IndexClosureFile(std::string_view path, angel_lsp::parser::AngelScriptParser& parser,
                              SyntaxRevisionCache* syntaxCache)
{
    const std::string uriStr = UriFromPath(path);
    uint64_t revision;
    {
        std::lock_guard lock(m_closureMutex);
        if (GetDocumentStore().IsOpen(uriStr))
        {
            return;
        }
        revision = ++m_nextClosedRevision;
        m_closureDocuments[uriStr] = {nullptr, revision};
        m_scopeIndex.InvalidateDocumentOccurrences(uriStr);
    }

    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.empty())
    {
        std::lock_guard lock(m_closureMutex);
        if (IsClosedRevisionCurrentLocked(uriStr, revision))
        {
            m_closureDocuments.erase(uriStr);
        }
        return;
    }

    auto prepared = PrepareAnalysisSource(uriStr, content);
    AnalyzeDocumentRequest request{
        .uriStr = uriStr, .text = std::move(prepared.text), .parser = parser, .closedRevision = revision};
    analysis::SymbolTable staging;
    auto parsed = ParseAndCollectSymbols(request, staging, syntaxCache);
    const auto* index = parsed.syntax ? &parsed.syntax->index : nullptr;
    auto scopes = (m_localScopeCollector && index) ? m_localScopeCollector->CollectScopesFromIndex(*index, request.text)
                                                   : nullptr;
    auto calls =
        scopes ? analysis::CollectCallsFromScopeTree(*scopes)
               : (index ? analysis::CollectCallsFromIndex(*index, request.text) : std::vector<analysis::CallSite>{});

    std::lock_guard lock(m_closureMutex);
    if (!IsClosedRevisionCurrentLocked(uriStr, revision) || GetDocumentStore().IsOpen(uriStr))
    {
        return;
    }
    m_symbolTable.ReplaceDocumentSymbols(uriStr, std::move(staging));
    auto& closed = m_closureDocuments[uriStr];
    closed.text = std::make_shared<const std::string>(std::move(content));
    if (parsed.syntax)
    {
        auto source =
            *closed.text == request.text ? closed.text : std::make_shared<const std::string>(std::move(request.text));
        closed.indexedSyntax = std::make_shared<const ClosedIndexedSyntax>(
            ClosedIndexedSyntax{std::move(source), std::move(parsed.diagnostics)});
        closed.lexicalScopes = std::move(scopes);
    }
    m_scopeIndex.SetScopeTree(uriStr, closed.lexicalScopes);
    m_callGraph.SetDocumentCalls(uriStr, std::move(calls));
    m_indexedUriByPath[std::string(path)] = uriStr;
    m_lastSavedInterfaceHash[uriStr] = m_symbolTable.ComputeDocumentInterfaceHash(uriStr);
}

void Server::IndexConfiguredModules(angel_lsp::parser::AngelScriptParser& parser)
{
    for (const auto& view : m_modules)
    {
        for (const auto& path : view.memberPaths)
        {
            const std::string uriStr = UriFromPath(path);

            if (IsOpenElsewhere(uriStr) || GetDocumentStore().IsOpen(uriStr))
            {
                continue;
            }

            bool alreadyIndexed = false;
            {
                std::lock_guard<std::mutex> lock(m_closureMutex);
                if (const auto indexed = m_indexedUriByPath.find(path);
                    indexed != m_indexedUriByPath.end() && indexed->second == uriStr)
                {
                    const auto cached = m_closureDocuments.find(uriStr);
                    alreadyIndexed = cached != m_closureDocuments.end() && cached->second.text;
                }
            }
            if (alreadyIndexed)
            {
                continue;
            }

            IndexClosureFile(path, parser);
        }
    }
    m_symbolTable.EnsureRuleIndex();
    SyncModuleIndexSymbols();
}

size_t Server::IndexModuleClosure(std::string_view openUriStr, SyntaxRevisionCache* syntaxCache)
{
    const std::string openPath = CanonicalPathFromUri(openUriStr);
    if (openPath.empty())
    {
        return 0;
    }

    std::string previousToPurge;
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        if (const auto previous = m_indexedUriByPath.find(openPath); previous != m_indexedUriByPath.end())
        {
            if (previous->second != openUriStr)
            {
                previousToPurge = previous->second;
            }
        }
        m_indexedUriByPath[openPath] = std::string(openUriStr);
    }
    if (!previousToPurge.empty())
    {
        PurgeClosureFile(previousToPurge);
    }

    std::vector<std::string> indexed;
    size_t newlyIndexed = 0;
    angel_lsp::parser::AngelScriptParser closureParser(m_logger.get());
    const std::vector<std::string> closure = ComputeModuleClosure(openPath);

    for (const auto& path : closure)
    {
        if (NormalizedPathsEqual(path, openPath))
        {
            continue;
        }

        std::string uriStr;
        bool alreadyOpen = false;
        bool alreadyIndexed = false;
        {
            std::lock_guard<std::mutex> lock(m_closureMutex);
            const auto already = m_indexedUriByPath.find(path);
            uriStr = (already != m_indexedUriByPath.end()) ? already->second : UriFromPath(path);
            alreadyOpen = GetDocumentStore().IsOpen(uriStr);
            alreadyIndexed = m_closureDocuments.contains(uriStr);
        }

        if (alreadyOpen)
        {
            continue;
        }

        if (!alreadyIndexed)
        {
            IndexClosureFile(path, closureParser, syntaxCache);
            ++newlyIndexed;
        }

        indexed.push_back(uriStr);
    }

    if (newlyIndexed > 0)
    {
        LogInfo(fmt::format("Indexed {} file(s) from the #include module of {}", newlyIndexed, openUriStr));
    }

    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        if (GetDocumentStore().IsOpen(openUriStr) || !m_closureDocuments.contains(std::string(openUriStr)))
        {
            m_openDocumentClosures[std::string(openUriStr)] = std::move(indexed);
        }
    }
    return newlyIndexed;
}
} // namespace angel_lsp
