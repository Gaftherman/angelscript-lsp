#include "lsp/Server.h"
#include "utils/Utils.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include "features/document_link/DocumentLinkHandler.h"
#include <spdlog/fmt/fmt.h>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace angel_lsp
{
    void Server::BuildModuleIndex()
    {
        std::vector<ModuleView> resolved;
        resolved.reserve(m_config.modules.size());

        const auto tellUser = [this](const std::string &text) {
            LogError(text);

            lsp::notifications::Window_ShowMessage::Params params;
            params.type = lsp::MessageType::Warning;
            params.message = text;

            std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
            m_messageHandler->sendNotification<lsp::notifications::Window_ShowMessage>(std::move(params));
        };

        const std::vector<std::string> workspaceFiles = m_includeGraph.AllFiles();

        for (const auto &definition : m_config.modules)
        {
            if (definition.name.empty() || (definition.entry.empty() && definition.folder.empty()))
            {
                tellUser("AngelScript: a configured module needs a name and at least one of an entry "
                         "script or a folder. One was skipped.");
                continue;
            }

            const bool duplicate = std::any_of(resolved.begin(), resolved.end(),
                                               [&definition](const ModuleView &existing)
                                               { return existing.name == definition.name; });
            if (duplicate)
            {
                tellUser(fmt::format(
                    "AngelScript: two modules are both named '{}'. The second was skipped - "
                    "'external shared' cannot say which module an entity came from otherwise.",
                    definition.name));
                continue;
            }

            ModuleView view;
            view.name = definition.name;

            if (!definition.folder.empty())
            {
                view.folderPath = ResolveConfiguredPath(definition.folder);

                std::error_code ec;
                if (!std::filesystem::is_directory(std::filesystem::path(view.folderPath), ec))
                {
                    tellUser(fmt::format(
                        "AngelScript: module '{}' names a folder that does not exist: {}",
                        view.name, view.folderPath));
                    continue;
                }

                for (const auto &candidate : workspaceFiles)
                {
                    if (PathIsInside(candidate, view.folderPath))
                        view.memberPaths.insert(candidate);
                }
            }

            if (!definition.entry.empty())
            {
                view.entryPath = ResolveConfiguredPath(definition.entry);

                std::error_code ec;
                if (!std::filesystem::is_regular_file(std::filesystem::path(view.entryPath), ec))
                {
                    tellUser(fmt::format(
                        "AngelScript: module '{}' names an entry script that does not exist: {}",
                        view.name, view.entryPath));
                    continue;
                }

                for (const auto &member : m_includeGraph.GetModuleClosure(view.entryPath))
                {
                    view.closurePaths.insert(member);
                    view.memberPaths.insert(member);
                }

                view.closurePaths.insert(view.entryPath);
                view.memberPaths.insert(view.entryPath);
            }

            LogInfo(fmt::format(
                "Module '{}': {} file(s){}{}", view.name, view.memberPaths.size(),
                view.folderPath.empty() ? std::string() : fmt::format(" under {}", view.folderPath),
                view.entryPath.empty() ? std::string() : fmt::format(" from {}", view.entryPath)));

            resolved.push_back(std::move(view));
        }

        m_modules = std::move(resolved);
    }

    bool Server::PathIsInside(const std::string &normalizedPath, const std::string &normalizedDirectory)
    {
        if (normalizedDirectory.empty() || normalizedPath.size() <= normalizedDirectory.size())
        {
            return false;
        }

        if (!PathsAreSameFile(normalizedPath.substr(0, normalizedDirectory.size()), normalizedDirectory))
        {
            return false;
        }

        return normalizedPath[normalizedDirectory.size()] == '/';
    }

    Server::ModuleClaim Server::ClaimFor(const std::string &normalizedPath) const
    {
        struct Candidate
        {
            const ModuleView *view;
            int rank;
            size_t depth;
        };

        std::vector<Candidate> candidates;

        for (const auto &view : m_modules)
        {
            if (view.closurePaths.contains(normalizedPath))
            {
                candidates.push_back(Candidate{ &view, 2, 0 });
            }
            else if (PathIsInside(normalizedPath, view.folderPath))
            {
                candidates.push_back(Candidate{ &view, 1, view.folderPath.size() });
            }
        }

        ModuleClaim claim;
        if (candidates.empty())
        {
            return claim;
        }

        const auto best = std::max_element(candidates.begin(), candidates.end(),
                                           [](const Candidate &a, const Candidate &b)
                                           {
                                               if (a.rank != b.rank) return a.rank < b.rank;
                                               return a.depth < b.depth;
                                           });

        claim.owner = best->view;

        for (const auto &candidate : candidates)
        {
            if (candidate.view != claim.owner)
                claim.alsoClaimedBy.push_back(candidate.view->name);
        }

        return claim;
    }

    void Server::RememberOpenDocument(const std::string &uriStr, const std::string &text)
    {
        std::lock_guard<std::mutex> lock(m_openSnapshotMutex);
        m_openSnapshot[uriStr] = text;
    }

    void Server::ForgetOpenDocument(const std::string &uriStr)
    {
        std::lock_guard<std::mutex> lock(m_openSnapshotMutex);
        m_openSnapshot.erase(uriStr);
    }

    bool Server::IsOpenElsewhere(const std::string &uriStr) const
    {
        std::lock_guard<std::mutex> lock(m_openSnapshotMutex);
        return m_openSnapshot.contains(uriStr);
    }

    void Server::ScheduleOpenDocumentsForReanalysis()
    {
        std::vector<std::pair<std::string, std::string>> open;
        {
            std::lock_guard<std::mutex> lock(m_openSnapshotMutex);
            open.reserve(m_openSnapshot.size());
            for (const auto &[uriStr, text] : m_openSnapshot)
            {
                if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
                {
                    continue;
                }
                open.emplace_back(uriStr, text);
            }
        }

        for (const auto &[uriStr, text] : open)
            ScheduleAnalysis(uriStr, text, /*force=*/true);

        if (m_logger)
        {
            LogInfo(fmt::format(
                "Re-analysing {} open document(s) now the workspace is indexed", open.size()));
        }
    }

    std::string Server::ResolveConfiguredPath(const std::string &configured) const
    {
        std::string cleaned = configured;
        for (const std::string_view prefix : {"${workspaceFolder}/", "${workspaceFolder}\\", "${workspaceRoot}/", "${workspaceRoot}\\"})
        {
            if (cleaned.rfind(prefix, 0) == 0)
            {
                cleaned = cleaned.substr(prefix.size());
                break;
            }
        }
        if (cleaned == "${workspaceFolder}" || cleaned == "${workspaceRoot}")
        {
            cleaned = ".";
        }

        const std::filesystem::path asWritten(cleaned);
        if (asWritten.is_absolute())
        {
            return angel_lsp::utils::IncludeResolver::NormalizePath(cleaned);
        }

        std::error_code ec;
        for (const auto &workspaceRoot : WorkspaceRoots())
        {
            const std::filesystem::path candidate =
                std::filesystem::path(angel_lsp::utils::UriToPath(workspaceRoot)) / asWritten;

            if (std::filesystem::exists(candidate, ec))
            {
                return angel_lsp::utils::IncludeResolver::NormalizePath(candidate.string());
            }
        }

        return angel_lsp::utils::IncludeResolver::NormalizePath(configured);
    }

    void Server::AnalyzeConfiguredModules()
    {
        if (m_modules.empty())
        {
            return;
        }

        for (const auto &view : m_modules)
        {
            for (const auto &path : view.memberPaths)
            {
                const std::string uriStr = UriFromPath(path);

                if (IsOpenElsewhere(uriStr))
                {
                    continue;
                }

                const ModuleClaim claim = ClaimFor(path);
                if (claim.owner == nullptr || claim.owner->name != view.name)
                {
                    continue;
                }

                std::ifstream file(path, std::ios::binary);
                if (!file.is_open())
                {
                    continue;
                }

                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

                {
                    std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
                    m_publishedForModules.insert(uriStr);
                }
                ScheduleAnalysis(uriStr, content);
            }
        }
    }

    void Server::WithdrawStaleModuleDiagnostics()
    {
        std::vector<std::string> stale;

        {
            std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
            for (const auto &uriStr : m_publishedForModules)
            {
                const std::string path = CanonicalPathFromUri(uriStr);
                if (path.empty() || ClaimFor(path).owner == nullptr)
                {
                    stale.push_back(uriStr);
                }
            }
        }

        for (const auto &uriStr : stale)
        {
            PublishDiagnostics(uriStr, std::string(), {});
            std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
            m_publishedForModules.erase(uriStr);
        }

        if (!stale.empty())
        {
            LogInfo(fmt::format("Withdrew module diagnostics for {} file(s)", stale.size()));
        }
    }

    void Server::PurgeUnusedClosureFiles()
    {
        ankerl::unordered_dense::set<std::string> wantedCanonicalPaths;

        {
            std::lock_guard<std::mutex> lock(m_openSnapshotMutex);
            for (const auto &[openUri, _] : m_openSnapshot)
            {
                const std::string p = CanonicalPathFromUri(openUri);
                if (!p.empty())
                {
                    wantedCanonicalPaths.insert(p);
                }
            }
        }

        for (const auto &[_, closureUris] : m_openDocumentClosures)
        {
            for (const auto &uri : closureUris)
            {
                const std::string p = CanonicalPathFromUri(uri);
                if (!p.empty())
                {
                    wantedCanonicalPaths.insert(p);
                }
            }
        }

        for (const auto &view : m_modules)
        {
            for (const auto &path : view.memberPaths)
            {
                const std::string p = angel_lsp::utils::IncludeResolver::NormalizePath(path);
                if (!p.empty())
                {
                    wantedCanonicalPaths.insert(p);
                }
            }
        }

        std::vector<std::string> toPurge;
        for (const auto &[uriStr, _] : m_closureDocuments)
        {
            const std::string p = CanonicalPathFromUri(uriStr);
            if (p.empty() || !wantedCanonicalPaths.contains(p))
            {
                toPurge.push_back(uriStr);
            }
        }

        for (const auto &[path, uriStr] : m_indexedUriByPath)
        {
            const std::string p = angel_lsp::utils::IncludeResolver::NormalizePath(path);
            if (!wantedCanonicalPaths.contains(p) && !IsOpenElsewhere(uriStr))
            {
                toPurge.push_back(uriStr);
            }
        }

        std::sort(toPurge.begin(), toPurge.end());
        toPurge.erase(std::unique(toPurge.begin(), toPurge.end()), toPurge.end());

        for (const auto &uriStr : toPurge)
        {
            PurgeClosureFile(uriStr);
        }

        if (!toPurge.empty())
        {
            LogInfo(fmt::format(
                "Purged {} stale closure file(s) no longer in any module or open closure", toPurge.size()));
        }
    }

    void Server::IndexConfiguredModules(angel_lsp::parser::AngelScriptParser &parser)
    {
        for (const auto &view : m_modules)
        {
            for (const auto &path : view.memberPaths)
            {
                const std::string uriStr = UriFromPath(path);

                if (IsOpenElsewhere(uriStr) || m_openDocuments.contains(uriStr))
                {
                    continue;
                }

                if (const auto indexed = m_indexedUriByPath.find(path);
                    indexed != m_indexedUriByPath.end() && indexed->second == uriStr)
                {
                    continue;
                }

                IndexClosureFile(path, parser);
                m_indexedUriByPath[path] = uriStr;
            }
        }
    }

    std::optional<angel_lsp::analysis::SemanticAnalysisRequest::ModuleContext>
    Server::ModuleContextFor(const std::string &uriStr) const
    {
        if (m_modules.empty())
        {
            return std::nullopt;
        }

        const std::string path = CanonicalPathFromUri(uriStr);
        if (path.empty())
        {
            return std::nullopt;
        }

        angel_lsp::analysis::SemanticAnalysisRequest::ModuleContext context;
        context.moduleNames.reserve(m_modules.size());
        for (const auto &view : m_modules)
            context.moduleNames.push_back(view.name);

        const ModuleClaim claim = ClaimFor(path);
        const ModuleView *owning = claim.owner;
        context.alsoClaimedBy = claim.alsoClaimedBy;

        if (owning == nullptr)
        {
            return context;
        }

        context.name = owning->name;

        m_symbolTable.ForEachSymbol(
            [owning, &context](const std::string &name, const std::vector<angel_lsp::analysis::Symbol> &symbols)
            {
                for (const auto &symbol : symbols)
                {
                    const bool declaresShared =
                        (symbol.type == angel_lsp::analysis::SymbolType::Class &&
                         symbol.GetClass().hasBraces && symbol.GetClass().modifiers.isShared &&
                         !symbol.GetClass().modifiers.isExternal) ||
                        (symbol.type == angel_lsp::analysis::SymbolType::Function &&
                         symbol.GetFunction().hasBody && symbol.GetFunction().modifiers.isShared &&
                         !symbol.GetFunction().modifiers.isExternal);

                    if (!declaresShared)
                        continue;

                    const std::string declaringPath = CanonicalPathFromUri(symbol.fileUri);
                    if (declaringPath.empty() || owning->memberPaths.contains(declaringPath))
                        continue;

                    context.sharedElsewhere.insert(name);
                    return;
                }
            });

        return context;
    }

    std::vector<std::string> Server::IncludeAllowedRoots() const
    {
        std::vector<std::string> roots;

        for (const auto &workspaceRoot : WorkspaceRoots())
        {
            std::string path = angel_lsp::utils::UriToPath(workspaceRoot);
            if (!path.empty())
                roots.push_back(std::move(path));
        }

        const auto searchDirectories = SearchDirectories();
        roots.insert(roots.end(), searchDirectories->begin(), searchDirectories->end());

        for (const auto &predefined : m_config.predefinedFiles)
        {
            std::error_code ec;
            std::filesystem::path configured(predefined);
            if (configured.has_parent_path())
                roots.push_back(configured.parent_path().string());
        }

        for (const auto &definition : m_config.modules)
        {
            if (!definition.folder.empty())
                roots.push_back(definition.folder);
        }

        return roots;
    }

    std::vector<angel_lsp::utils::ExcludedLineRange> Server::ExcludedLineRanges(const std::string &text) const
    {
        return angel_lsp::utils::FindExcludedLineRanges(text, *DefinedWords(), m_config.preprocessor);
    }

    void Server::IndexClosureFile(const std::string &path, angel_lsp::parser::AngelScriptParser &parser)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return;

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (content.empty())
            return;

        const std::string uriStr = UriFromPath(path);

        document::TreePtr tree = document::MakeTreePtr(parser.Parse(content));

        ReplaceSymbolsFromTree(uriStr, content, tree.get());

        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);
        if (tree)
        {
            m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(tree.get()), content));
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(tree.get()), content));
        }

        m_closureDocuments[uriStr] = std::move(content);
        m_indexedUriByPath[path] = uriStr;
    }

    void Server::AppendIncludeDiagnostics(const std::string &uriStr, const std::string &text, std::vector<angel_lsp::analysis::Diagnostic> &diagnostics) const
    {
        if (!m_config.features.enableDocumentLink)
            return;

        const auto searchDirectories = SearchDirectories();

        features::DocumentLinkRequest request{ uriStr, text, *searchDirectories, m_i18n.get(), IncludeAllowedRoots() };

        request.excludedLineRanges = ExcludedLineRanges(text);
        request.implicitExtension = std::string(ImplicitIncludeExtension());

        auto includeDiagnostics = features::GetUnresolvedIncludeDiagnostics(request);
        diagnostics.insert(diagnostics.end(), includeDiagnostics.begin(), includeDiagnostics.end());
    }

    void Server::PurgeClosureFile(const std::string &uriStr)
    {
        {
            std::lock_guard<std::mutex> lock(m_diagnosticsCacheMutex);
            m_diagnosticsCache.erase(uriStr);
        }

        m_symbolTable.ClearDocumentSymbols(uriStr);
        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);
        m_closureDocuments.erase(uriStr);

        const std::string path = CanonicalPathFromUri(uriStr);
        if (!path.empty())
        {
            if (const auto it = m_indexedUriByPath.find(path); it != m_indexedUriByPath.end() && it->second == uriStr)
                m_indexedUriByPath.erase(it);
        }
    }

    void Server::IndexModuleClosure(const std::string &openUriStr)
    {
        const std::string openPath = CanonicalPathFromUri(openUriStr);
        if (openPath.empty())
            return;

        if (const auto previous = m_indexedUriByPath.find(openPath); previous != m_indexedUriByPath.end())
        {
            if (previous->second != openUriStr)
                PurgeClosureFile(previous->second);
        }
        m_indexedUriByPath[openPath] = openUriStr;

        std::vector<std::string> indexed;
        size_t newlyIndexed = 0;
        angel_lsp::parser::AngelScriptParser closureParser(m_logger.get());

        for (const auto &path : m_includeGraph.GetModuleClosure(openPath))
        {
            if (path == openPath)
                continue;

            const std::string uriStr = UriFromPath(path);

            if (m_openDocuments.contains(uriStr))
                continue;

            if (const auto already = m_indexedUriByPath.find(path);
                already != m_indexedUriByPath.end() && m_openDocuments.contains(already->second))
            {
                continue;
            }

            if (!m_closureDocuments.contains(uriStr))
            {
                IndexClosureFile(path, closureParser);
                ++newlyIndexed;
            }

            indexed.push_back(uriStr);
        }

        if (newlyIndexed > 0)
        {
            LogInfo(fmt::format("Indexed {} file(s) from the #include module of {}", newlyIndexed, openUriStr));
        }

        m_openDocumentClosures[openUriStr] = std::move(indexed);
    }

    void Server::ReleaseModuleClosure(const std::string &openUriStr)
    {
        const auto closure = m_openDocumentClosures.find(openUriStr);
        if (closure == m_openDocumentClosures.end())
            return;

        const std::vector<std::string> released = std::move(closure->second);
        m_openDocumentClosures.erase(closure);

        for (const auto &uriStr : released)
        {
            bool stillNeeded = false;
            for (const auto &[otherUri, otherClosure] : m_openDocumentClosures)
            {
                if (std::find(otherClosure.begin(), otherClosure.end(), uriStr) != otherClosure.end())
                {
                    stillNeeded = true;
                    break;
                }
            }

            if (!stillNeeded)
            {
                const std::string path = CanonicalPathFromUri(uriStr);
                if (!path.empty() && ClaimFor(path).owner != nullptr)
                {
                    continue;
                }

                PurgeClosureFile(uriStr);
            }
        }
    }
}
