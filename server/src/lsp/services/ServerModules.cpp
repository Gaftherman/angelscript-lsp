#include "analysis/rules/RuleIndex.h"
#include "features/document_link/DocumentLinkHandler.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Utils.h"
#include "utils/WorkspaceScan.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
void Server::NotifyModuleWarning(const std::string& text)
{
    LogError(text);

    lsp::notifications::Window_ShowMessage::Params params;
    params.type = lsp::MessageType::Warning;
    params.message = text;

    std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
    m_messageHandler->sendNotification<lsp::notifications::Window_ShowMessage>(std::move(params));
}

static bool PathIsInside(const std::string& normalizedPath, const std::string& normalizedDirectory);

static bool PopulateModuleFolderMembers(Server& server, const std::string& folder, ModuleView& view,
                                        const std::vector<std::string>& workspaceFiles)
{
    view.folderPath = server.ResolveConfiguredPath(folder);

    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(view.folderPath), ec))
    {
        server.NotifyModuleWarning(
            fmt::format("AngelScript: module '{}' names a folder that does not exist: {}", view.name, view.folderPath));
        return false;
    }

    for (const auto& candidate : workspaceFiles)
    {
        if (PathIsInside(candidate, view.folderPath))
        {
            view.memberPaths.insert(candidate);
        }
    }

    if (view.memberPaths.empty())
    {
        angel_lsp::utils::ForEachWorkspaceFile(
            {view.folderPath}, server.GetConfig().exclude, nullptr,
            [&](const std::filesystem::directory_entry& entry)
            {
                const std::string pathStr = entry.path().string();
                if (!server.GetConfig().info.fileExtension.empty() &&
                    std::string_view(pathStr).ends_with(server.GetConfig().info.fileExtension))
                {
                    view.memberPaths.insert(angel_lsp::utils::IncludeResolver::NormalizeWalkedPath(entry.path()));
                }
            });
    }

    return true;
}

static bool PopulateModuleEntryClosure(Server& server, const std::string& entry, ModuleView& view)
{
    view.entryPath = server.ResolveConfiguredPath(entry);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(view.entryPath), ec))
    {
        server.NotifyModuleWarning(fmt::format("AngelScript: module '{}' names an entry script that does not exist: {}",
                                               view.name, view.entryPath));
        return false;
    }

    for (const auto& member : server.GetIncludeGraph().GetForwardClosure(view.entryPath))
    {
        view.closurePaths.insert(member);
        view.memberPaths.insert(member);
    }

    if (view.closurePaths.empty())
    {
        const auto searchDirs = server.SearchDirectories();
        for (const auto& member : angel_lsp::utils::IncludeResolver::ResolveAllIncludes(view.entryPath, *searchDirs, {},
                                                                                        server.IncludeAllowedRoots()))
        {
            view.closurePaths.insert(member);
            view.memberPaths.insert(member);
        }
    }

    view.closurePaths.insert(view.entryPath);
    view.memberPaths.insert(view.entryPath);

    for (const auto& forceFile : server.GetConfig().forceIncludeFiles)
    {
        const std::string resolvedForce = server.ResolveConfiguredPath(forceFile);
        if (!resolvedForce.empty())
        {
            view.closurePaths.insert(resolvedForce);
            view.memberPaths.insert(resolvedForce);
            for (const auto& fwd : server.GetIncludeGraph().GetForwardClosure(resolvedForce))
            {
                view.closurePaths.insert(fwd);
                view.memberPaths.insert(fwd);
            }
        }
    }
    return true;
}

static std::optional<ModuleView> ResolveModuleDefinition(Server& server,
                                                         const config::ServerConfig::ModuleDefinition& definition,
                                                         const std::vector<ModuleView>& resolved,
                                                         const std::vector<std::string>& workspaceFiles)
{
    if (definition.name.empty() || (definition.entry.empty() && definition.folder.empty()))
    {
        server.NotifyModuleWarning("AngelScript: a configured module needs a name and at least one of an entry "
                                   "script or a folder. One was skipped.");
        return std::nullopt;
    }

    const bool duplicate = std::any_of(resolved.begin(), resolved.end(), [&definition](const ModuleView& existing)
                                       { return existing.name == definition.name; });
    if (duplicate)
    {
        server.NotifyModuleWarning(
            fmt::format("AngelScript: two modules are both named '{}'. The second was skipped - "
                        "'external shared' cannot say which module an entity came from otherwise.",
                        definition.name));
        return std::nullopt;
    }

    ModuleView view;
    view.name = definition.name;
    view.isImplicitEntryPoint = definition.isImplicitEntryPoint;

    if (!definition.folder.empty() && !PopulateModuleFolderMembers(server, definition.folder, view, workspaceFiles))
    {
        return std::nullopt;
    }

    if (!definition.entry.empty() && !PopulateModuleEntryClosure(server, definition.entry, view))
    {
        return std::nullopt;
    }

    server.LogInfo(fmt::format("Module '{}': {} file(s){}{}", view.name, view.memberPaths.size(),
                               view.folderPath.empty() ? std::string() : fmt::format(" under {}", view.folderPath),
                               view.entryPath.empty() ? std::string() : fmt::format(" from {}", view.entryPath)));

    return view;
}

static bool NormalizedPathsEqual(std::string_view a, std::string_view b)
{
    if (a == b)
    {
        return true;
    }
#if defined(_WIN32)
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
#else
    return false;
#endif
}

static void AppendUniqueDirectory(std::vector<std::string>& dirs, const std::string& dir, bool& added)
{
    if (dir.empty())
    {
        return;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(dir), ec) &&
        std::find(dirs.begin(), dirs.end(), dir) == dirs.end())
    {
        dirs.push_back(dir);
        added = true;
    }
}

void Server::AugmentSearchDirectoriesFromModules()
{
    std::vector<std::string> searchDirs = *SearchDirectories();
    bool addedSearchDir = false;

    auto appendPathOrParent = [&](const std::string& rawPath, bool takeParent)
    {
        if (rawPath.empty())
        {
            return;
        }
        std::filesystem::path p(rawPath);
        if (p.is_absolute())
        {
            AppendUniqueDirectory(searchDirs, takeParent ? p.parent_path().string() : p.string(), addedSearchDir);
        }
        else
        {
            for (const auto& wsRoot : WorkspaceRoots())
            {
                const std::string wsPath = angel_lsp::utils::UriToPath(wsRoot);
                if (!wsPath.empty())
                {
                    const auto fullP = std::filesystem::path(wsPath) / p;
                    AppendUniqueDirectory(searchDirs, takeParent ? fullP.parent_path().string() : fullP.string(),
                                          addedSearchDir);
                }
            }
        }
    };

    for (const auto& view : m_modules)
    {
        if (!view.folderPath.empty())
        {
            AppendUniqueDirectory(searchDirs, view.folderPath, addedSearchDir);
        }
        if (!view.entryPath.empty())
        {
            const std::filesystem::path ep(view.entryPath);
            if (ep.has_parent_path())
            {
                AppendUniqueDirectory(searchDirs, ep.parent_path().string(), addedSearchDir);
            }
        }
    }

    for (const auto& def : m_config.modules)
    {
        appendPathOrParent(def.folder, false);
        appendPathOrParent(def.entry, true);
    }
    appendPathOrParent(m_config.moduleEntryPoint, true);

    for (const auto& wsRoot : WorkspaceRoots())
    {
        const std::string wsPath = angel_lsp::utils::UriToPath(wsRoot);
        if (!wsPath.empty())
        {
            AppendUniqueDirectory(searchDirs, (std::filesystem::path(wsPath) / "maps").string(), addedSearchDir);
        }
    }

    if (addedSearchDir)
    {
        m_workspaceStore->SetSearchDirectories(std::move(searchDirs));
    }
}

void Server::BuildModuleIndex()
{
    std::vector<config::ServerConfig::ModuleDefinition> allDefs = m_config.modules;
    if (!m_config.moduleEntryPoint.empty())
    {
        const bool exists = std::any_of(allDefs.begin(), allDefs.end(),
                                        [this](const auto& def) { return def.entry == m_config.moduleEntryPoint; });
        if (!exists)
        {
            config::ServerConfig::ModuleDefinition entryDef;
            entryDef.name = "default";
            entryDef.entry = m_config.moduleEntryPoint;
            entryDef.isImplicitEntryPoint = true;
            allDefs.push_back(std::move(entryDef));
        }
    }

    std::vector<ModuleView> resolved;
    resolved.reserve(allDefs.size());

    const std::vector<std::string> workspaceFiles = GetIncludeGraph().AllFiles();

    for (const auto& definition : allDefs)
    {
        if (auto view = ResolveModuleDefinition(*this, definition, resolved, workspaceFiles))
        {
            resolved.push_back(std::move(*view));
        }
    }

    m_modules = std::move(resolved);

    AugmentSearchDirectoriesFromModules();
    SyncModuleIndexSymbols();
}

static bool PathIsInside(const std::string& normalizedPath, const std::string& normalizedDirectory)
{
    if (normalizedDirectory.empty() || normalizedPath.size() <= normalizedDirectory.size() ||
        normalizedPath[normalizedDirectory.size()] != '/')
    {
        return false;
    }

    return NormalizedPathsEqual(std::string_view(normalizedPath).substr(0, normalizedDirectory.size()),
                                normalizedDirectory);
}

ModuleClaim Server::ClaimFor(const std::string& normalizedPath) const
{
    std::vector<ModuleCandidate> candidates;

    for (const auto& view : m_modules)
    {
        if (view.closurePaths.contains(normalizedPath))
        {
            candidates.push_back(ModuleCandidate{&view, 2, 0});
        }
        else if (PathIsInside(normalizedPath, view.folderPath))
        {
            candidates.push_back(ModuleCandidate{&view, 1, view.folderPath.size()});
        }
    }

    ModuleClaim claim;
    if (candidates.empty())
    {
        return claim;
    }

    const auto best = std::max_element(candidates.begin(), candidates.end(),
                                       [](const ModuleCandidate& a, const ModuleCandidate& b)
                                       {
                                           if (a.rank != b.rank)
                                               return a.rank < b.rank;
                                           return a.depth < b.depth;
                                       });

    claim.owner = best->view;

    for (const auto& candidate : candidates)
    {
        if (candidate.view != claim.owner)
            claim.alsoClaimedBy.push_back(candidate.view->name);
    }

    return claim;
}

bool Server::IsOpenElsewhere(const std::string& uriStr) const
{
    return GetDocumentStore().IsOpen(uriStr);
}

void Server::ScheduleOpenDocumentsForReanalysis()
{
    const auto open = GetDocumentStore().GetSnapshot();
    for (const auto& [uriStr, text] : open)
    {
        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            continue;
        }
        ScheduleAnalysis(uriStr, text, true);
    }

    if (m_logger)
    {
        LogInfo(fmt::format("Re-analysing {} open document(s) now the workspace is indexed", open.size()));
    }
}

std::string Server::ResolveConfiguredPath(const std::string& configured) const
{
    std::string cleaned = configured;
    for (const std::string_view prefix :
         {"${workspaceFolder}/", "${workspaceFolder}\\", "${workspaceRoot}/", "${workspaceRoot}\\"})
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
    for (const auto& workspaceRoot : WorkspaceRoots())
    {
        const std::filesystem::path candidate =
            std::filesystem::path(angel_lsp::utils::UriToPath(workspaceRoot)) / asWritten;

        if (std::filesystem::exists(candidate, ec))
        {
            return angel_lsp::utils::IncludeResolver::NormalizePath(candidate.string());
        }
    }

    if (!WorkspaceRoots().empty())
    {
        const std::filesystem::path fallback =
            std::filesystem::path(angel_lsp::utils::UriToPath(WorkspaceRoots().front())) / asWritten;
        return angel_lsp::utils::IncludeResolver::NormalizePath(fallback.string());
    }

    return angel_lsp::utils::IncludeResolver::NormalizePath(configured);
}

void Server::AnalyzeConfiguredModules()
{
    if (m_modules.empty())
    {
        return;
    }

    for (const auto& view : m_modules)
    {
        if (view.isImplicitEntryPoint)
        {
            continue;
        }

        for (const auto& path : view.memberPaths)
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

            std::string content;
            {
                std::lock_guard<std::mutex> lock(m_closureMutex);
                if (const auto cached = m_closureDocuments.find(uriStr); cached != m_closureDocuments.end())
                {
                    content = cached->second;
                }
            }
            if (content.empty())
            {
                std::ifstream file(path, std::ios::binary);
                if (!file.is_open())
                {
                    continue;
                }
                content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            }

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
    ankerl::unordered_dense::set<std::string> stale;

    {
        std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
        for (const auto& uriStr : m_publishedForModules)
        {
            const std::string path = CanonicalPathFromUri(uriStr);
            if (path.empty() || ClaimFor(path).owner == nullptr)
            {
                stale.insert(uriStr);
            }
        }
    }

    if (!m_config.modules.empty())
    {
        for (const auto& uriStr : GetDocumentStore().GetOpenUris())
        {
            const std::string path = CanonicalPathFromUri(uriStr);
            if (path.empty() || ClaimFor(path).owner == nullptr)
            {
                stale.insert(uriStr);
            }
        }
    }

    for (const auto& uriStr : stale)
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

std::string Server::ReadClosedDocumentContent(const std::string& uriStr)
{
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        if (const auto cached = m_closureDocuments.find(uriStr); cached != m_closureDocuments.end())
        {
            return cached->second;
        }
    }
    const std::string path = CanonicalPathFromUri(uriStr);
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void Server::ReanalyzeClosedModuleDependents(const std::string& savedUriStr, const ModuleClaim& savedClaim,
                                             const ankerl::unordered_dense::set<std::string>& closureSet)
{
    std::vector<std::string> closedToReanalyze;
    {
        std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
        for (const auto& pubUri : m_publishedForModules)
        {
            if (pubUri == savedUriStr || GetDocumentStore().IsOpen(pubUri))
            {
                continue;
            }
            const std::string pubPath = CanonicalPathFromUri(pubUri);
            if (pubPath.empty())
            {
                continue;
            }
            const bool sameModule = (savedClaim.owner != nullptr && ClaimFor(pubPath).owner == savedClaim.owner);
            if (sameModule || closureSet.contains(pubPath))
            {
                closedToReanalyze.push_back(pubUri);
            }
        }
    }

    for (const auto& pubUri : closedToReanalyze)
    {
        std::string content = ReadClosedDocumentContent(pubUri);
        if (!content.empty())
        {
            ScheduleAnalysis(pubUri, content);
        }
    }
}

ankerl::unordered_dense::set<std::string> Server::CollectWantedClosurePaths() const
{
    ankerl::unordered_dense::set<std::string> wantedCanonicalPaths;

    for (const auto& openUri : GetDocumentStore().GetOpenUris())
    {
        const std::string p = CanonicalPathFromUri(openUri);
        if (!p.empty())
        {
            wantedCanonicalPaths.insert(p);
        }
    }

    for (const auto& [_, closureUris] : m_openDocumentClosures)
    {
        for (const auto& uri : closureUris)
        {
            const std::string p = CanonicalPathFromUri(uri);
            if (!p.empty())
            {
                wantedCanonicalPaths.insert(p);
            }
        }
    }

    for (const auto& view : m_modules)
    {
        for (const auto& path : view.memberPaths)
        {
            const std::string p = angel_lsp::utils::IncludeResolver::NormalizePath(path);
            if (!p.empty())
            {
                wantedCanonicalPaths.insert(p);
            }
        }
    }

    return wantedCanonicalPaths;
}

std::vector<std::string>
Server::CollectStaleClosureUris(const ankerl::unordered_dense::set<std::string>& wantedPaths) const
{
    std::vector<std::string> toPurge;
    std::lock_guard<std::mutex> lock(m_closureMutex);
    for (const auto& [uriStr, _] : m_closureDocuments)
    {
        const std::string p = CanonicalPathFromUri(uriStr);
        if (p.empty() || !wantedPaths.contains(p))
        {
            toPurge.push_back(uriStr);
        }
    }

    for (const auto& [path, uriStr] : m_indexedUriByPath)
    {
        const std::string p = angel_lsp::utils::IncludeResolver::NormalizePath(path);
        if (!wantedPaths.contains(p) && !IsOpenElsewhere(uriStr))
        {
            toPurge.push_back(uriStr);
        }
    }

    std::sort(toPurge.begin(), toPurge.end());
    toPurge.erase(std::unique(toPurge.begin(), toPurge.end()), toPurge.end());
    return toPurge;
}

void Server::PurgeUnusedClosureFiles()
{
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    const auto wantedCanonicalPaths = CollectWantedClosurePaths();
    const auto toPurge = CollectStaleClosureUris(wantedCanonicalPaths);

    for (const auto& uriStr : toPurge)
    {
        PurgeClosureFile(uriStr);
    }

    if (!toPurge.empty())
    {
        LogInfo(fmt::format("Purged {} stale closure file(s) no longer in any module or open closure", toPurge.size()));
    }
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
                    alreadyIndexed = true;
                }
            }
            if (alreadyIndexed)
            {
                continue;
            }

            IndexClosureFile(path, parser);
            {
                std::lock_guard<std::mutex> lock(m_closureMutex);
                m_indexedUriByPath[path] = uriStr;
            }
        }
    }
    m_symbolTable.EnsureRuleIndex();
    SyncModuleIndexSymbols();
}

static bool SymbolDeclaresShared(const angel_lsp::analysis::Symbol& symbol)
{
    if (symbol.type == angel_lsp::analysis::SymbolType::Class)
    {
        const auto& c = symbol.GetClass();
        return c.hasBraces && c.modifiers.isShared && !c.modifiers.isExternal;
    }
    if (symbol.type == angel_lsp::analysis::SymbolType::Function)
    {
        const auto& f = symbol.GetFunction();
        return f.hasBody && f.modifiers.isShared && !f.modifiers.isExternal;
    }
    return false;
}

void Server::SyncModuleIndexSymbols()
{
    std::vector<ModuleIndex::ExportedSymbol> exported;
    for (const auto& view : m_modules)
    {
        exported.push_back(ModuleIndex::ExportedSymbol{view.name, view.folderPath, "", 0, 0});
    }
    if (const auto ruleIndex = m_symbolTable.GetRuleIndex())
    {
        for (const auto& [key, _] : ruleIndex->sharedDeclarationFilesByKey)
        {
            auto symsPtr = m_symbolTable.FindSymbolsPtr(key);
            if (!symsPtr)
            {
                continue;
            }
            for (const auto& sym : *symsPtr)
            {
                if (SymbolDeclaresShared(sym))
                {
                    exported.push_back(ModuleIndex::ExportedSymbol{sym.name, sym.containerName, sym.fileUri,
                                                                   sym.startLine, sym.startCharacter});
                }
            }
        }
    }
    GetModuleIndex().SetExportedSymbols(std::move(exported));
}

void Server::CollectSharedSymbolsElsewhere(const ModuleView& owning,
                                           ankerl::unordered_dense::set<std::string>& outShared) const
{
    const auto ruleIndex = m_symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    for (const auto& [name, fileUris] : ruleIndex->sharedDeclarationFilesByKey)
    {
        for (const auto& fileUri : fileUris)
        {
            const std::string declaringPath = CanonicalPathFromUri(fileUri);
            if (!declaringPath.empty() && !owning.memberPaths.contains(declaringPath))
            {
                outShared.insert(name);
                break;
            }
        }
    }
}

std::optional<angel_lsp::analysis::SemanticAnalysisRequest::ModuleContext>
Server::ModuleContextFor(const std::string& uriStr) const
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
    for (const auto& view : m_modules)
    {
        context.moduleNames.push_back(view.name);
    }

    const ModuleClaim claim = ClaimFor(path);
    const ModuleView* owning = claim.owner;
    context.alsoClaimedBy = claim.alsoClaimedBy;

    if (owning == nullptr)
    {
        return context;
    }

    context.name = owning->name;
    CollectSharedSymbolsElsewhere(*owning, context.sharedElsewhere);

    return context;
}

static void AppendConfiguredPathAndParent(const Server& server, const std::string& raw, std::vector<std::string>& roots)
{
    if (raw.empty())
    {
        return;
    }
    const std::string resolved = server.ResolveConfiguredPath(raw);
    if (resolved.empty())
    {
        return;
    }
    roots.push_back(resolved);
    const std::filesystem::path p(resolved);
    if (p.has_parent_path())
    {
        roots.push_back(p.parent_path().string());
    }
}

static void CollectConfiguredAllowedRoots(const Server& server, const angel_lsp::config::ServerConfig& config,
                                          std::vector<std::string>& roots)
{
    for (const auto& predefined : config.predefinedFiles)
    {
        AppendConfiguredPathAndParent(server, predefined, roots);
    }

    if (!config.activePredefined.empty() && config.activePredefined != "all")
    {
        AppendConfiguredPathAndParent(server, config.activePredefined, roots);
    }

    for (const auto& definition : config.modules)
    {
        AppendConfiguredPathAndParent(server, definition.entry, roots);
        AppendConfiguredPathAndParent(server, definition.folder, roots);
    }

    AppendConfiguredPathAndParent(server, config.moduleEntryPoint, roots);

    for (const auto& forceFile : config.forceIncludeFiles)
    {
        AppendConfiguredPathAndParent(server, forceFile, roots);
    }
}

std::vector<std::string> Server::IncludeAllowedRoots() const
{
    std::vector<std::string> roots;

    for (const auto& workspaceRoot : WorkspaceRoots())
    {
        std::string path = angel_lsp::utils::UriToPath(workspaceRoot);
        if (!path.empty())
        {
            roots.push_back(path);
            const std::string norm = angel_lsp::utils::IncludeResolver::NormalizePath(path);
            if (!norm.empty() && norm != path)
            {
                roots.push_back(norm);
            }
        }
    }

    const auto searchDirectories = SearchDirectories();
    roots.insert(roots.end(), searchDirectories->begin(), searchDirectories->end());

    CollectConfiguredAllowedRoots(*this, m_config, roots);

    if (roots.empty())
    {
        for (const auto& uri : GetDocumentStore().GetOpenUris())
        {
            const std::string p = CanonicalPathFromUri(uri);
            if (!p.empty())
            {
                std::filesystem::path fp(p);
                if (fp.has_parent_path())
                {
                    roots.push_back(fp.parent_path().string());
                }
            }
        }
    }

    return roots;
}

std::vector<angel_lsp::utils::ExcludedLineRange> Server::ExcludedLineRanges(const std::string& text) const
{
    return angel_lsp::utils::FindExcludedLineRanges(text, *DefinedWords(), m_config.preprocessor);
}

void Server::IndexClosureFile(const std::string& path, angel_lsp::parser::AngelScriptParser& parser)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.empty())
        return;

    const std::string uriStr = UriFromPath(path);
    const std::string analysisText = AnalysisTextFor(uriStr, content);

    document::TreePtr tree = document::MakeTreePtr(parser.Parse(analysisText));

    ReplaceSymbolsFromTree(uriStr, analysisText, tree.get());

    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);
    if (tree)
    {
        auto scopes = m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(tree.get()), analysisText);
        if (scopes)
        {
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCallsFromScopeTree(*scopes));
            m_scopeIndex.SetScopeTree(uriStr, std::move(scopes));
        }
        else
        {
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(tree.get()), analysisText));
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        m_closureDocuments[uriStr] = std::move(content);
        m_indexedUriByPath[path] = uriStr;
        m_lastSavedInterfaceHash[uriStr] = m_symbolTable.ComputeDocumentInterfaceHash(uriStr);
    }
}

void Server::AppendIncludeDiagnostics(const std::string& uriStr, const std::string& text,
                                      std::vector<angel_lsp::analysis::Diagnostic>& diagnostics) const
{
    if (!m_config.features.enableDocumentLink)
        return;

    const auto searchDirectories = SearchDirectories();

    features::DocumentLinkRequest request{uriStr, text, *searchDirectories, m_i18n.get()};
    request.allowedRoots = IncludeAllowedRoots();
    request.excludedLineRanges = ExcludedLineRanges(text);
    request.implicitExtension = std::string(ImplicitIncludeExtension());

    auto includeDiagnostics = features::GetUnresolvedIncludeDiagnostics(request);
    diagnostics.insert(diagnostics.end(), includeDiagnostics.begin(), includeDiagnostics.end());
}

void Server::PurgeClosureFile(const std::string& uriStr)
{
    {
        std::lock_guard<std::mutex> lock(m_diagnosticsPipeline->GetCacheMutexRef());
        m_diagnosticsPipeline->GetCacheRef().erase(uriStr);
    }

    m_symbolTable.ClearDocumentSymbols(uriStr);
    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);

    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        m_closureDocuments.erase(uriStr);

        const std::string path = CanonicalPathFromUri(uriStr);
        if (!path.empty())
        {
            if (const auto it = m_indexedUriByPath.find(path); it != m_indexedUriByPath.end() && it->second == uriStr)
                m_indexedUriByPath.erase(it);
        }
    }
}

static void AppendFilteredMembers(const ankerl::unordered_dense::set<std::string>& members, const std::string& openPath,
                                  std::vector<std::string>& outClosure)
{
    for (const auto& member : members)
    {
        if (!NormalizedPathsEqual(member, openPath))
        {
            outClosure.push_back(member);
        }
    }
}

static void AppendEnclosingFolderModuleMembers(const std::vector<ModuleView>& modules, const std::string& openPath,
                                               const ModuleClaim& claim, std::vector<std::string>& closure)
{
    for (const auto& mod : modules)
    {
        if (claim.owner != nullptr && mod.name == claim.owner->name)
        {
            continue;
        }
        if (!mod.folderPath.empty() && PathIsInside(openPath, mod.folderPath))
        {
            AppendFilteredMembers(mod.memberPaths, openPath, closure);
        }
    }
}

static void AppendIncludeGraphClosure(const Server& server, const std::string& openPath,
                                      std::vector<std::string>& closure)
{
    if (!server.GetConfig().moduleEntryPoint.empty())
    {
        const std::string entryPath = server.ResolveConfiguredPath(server.GetConfig().moduleEntryPoint);
        auto fwd = server.GetIncludeGraph().GetForwardClosure(entryPath);
        if (std::any_of(fwd.begin(), fwd.end(),
                        [&openPath](const std::string& p) { return NormalizedPathsEqual(p, openPath); }))
        {
            closure.insert(closure.end(), fwd.begin(), fwd.end());
            return;
        }
    }
    const auto mc = server.GetIncludeGraph().GetModuleClosure(openPath);
    closure.insert(closure.end(), mc.begin(), mc.end());
}

static void AppendForceIncludesClosure(const Server& server, std::vector<std::string>& closure)
{
    for (const auto& forceFile : server.GetConfig().forceIncludeFiles)
    {
        const std::string resolvedForce = server.ResolveConfiguredPath(forceFile);
        if (resolvedForce.empty())
        {
            continue;
        }
        closure.push_back(resolvedForce);
        for (const auto& fwd : server.GetIncludeGraph().GetForwardClosure(resolvedForce))
        {
            closure.push_back(fwd);
        }
    }
}

std::vector<std::string> Server::ComputeModuleClosure(const std::string& openPath) const
{
    const ModuleClaim claim = ClaimFor(openPath);
    std::vector<std::string> closure;

    if (claim.owner != nullptr)
    {
        closure.reserve(claim.owner->memberPaths.size());
        AppendFilteredMembers(claim.owner->memberPaths, openPath, closure);
    }

    AppendEnclosingFolderModuleMembers(m_modules, openPath, claim, closure);
    AppendIncludeGraphClosure(*this, openPath, closure);
    AppendForceIncludesClosure(*this, closure);

    return closure;
}

size_t Server::IndexModuleClosure(const std::string& openUriStr)
{
    const std::string openPath = CanonicalPathFromUri(openUriStr);
    if (openPath.empty())
        return 0;

    std::string previousToPurge;
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        if (const auto previous = m_indexedUriByPath.find(openPath); previous != m_indexedUriByPath.end())
        {
            if (previous->second != openUriStr)
                previousToPurge = previous->second;
        }
        m_indexedUriByPath[openPath] = openUriStr;
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
            continue;

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
            continue;

        if (!alreadyIndexed)
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

    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        if (GetDocumentStore().IsOpen(openUriStr) || !m_closureDocuments.contains(openUriStr))
        {
            m_openDocumentClosures[openUriStr] = std::move(indexed);
        }
    }
    return newlyIndexed;
}

void Server::ReleaseModuleClosure(const std::string& openUriStr)
{
    std::vector<std::string> released;
    std::vector<std::string> toPurge;
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        const auto closure = m_openDocumentClosures.find(openUriStr);
        if (closure == m_openDocumentClosures.end())
            return;

        released = std::move(closure->second);
        m_openDocumentClosures.erase(closure);

        for (const auto& uriStr : released)
        {
            bool stillNeeded = false;
            for (const auto& [otherUri, otherClosure] : m_openDocumentClosures)
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

                toPurge.push_back(uriStr);
            }
        }
    }

    for (const auto& uriStr : toPurge)
    {
        PurgeClosureFile(uriStr);
    }
}
} // namespace angel_lsp
