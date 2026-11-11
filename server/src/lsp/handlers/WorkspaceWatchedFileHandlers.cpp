#include "lsp/Server.h"
#include "parser/AngelScriptParser.h"
#include "utils/IncludeResolver.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{

void Server::HandleNotificationsWorkspace_DidChangeWorkspaceFolders(
    lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params&& params)
{
    size_t rootCount = 0;
    {
        auto roots = m_workspaceStore->GetWorkspaceRoots();

        for (const auto& removed : params.event.removed)
        {
            const std::string root = angel_lsp::utils::IncludeResolver::NormalizePath(removed.uri.fsPath());
            std::erase(roots, root);
        }

        for (const auto& added : params.event.added)
        {
            const std::string root = angel_lsp::utils::IncludeResolver::NormalizePath(added.uri.fsPath());
            if (std::find(roots.begin(), roots.end(), root) == roots.end())
            {
                roots.push_back(root);
            }
        }

        rootCount = roots.size();
        m_workspaceStore->SetWorkspaceRoots(roots);
    }

    LogInfo(fmt::format("Workspace folders changed (+{} -{}); now {} root(s), rescanning", params.event.added.size(),
                        params.event.removed.size(), rootCount));

    // A rescan rather than an incremental patch: the include graph is rebuilt wholesale by
    // Build(), and a removed root's files have to leave the graph as much as an added root's
    // have to enter it.
    RestartWorkspaceScan();
}

bool Server::HandleWatchedFileDeleted(const std::string& path, const std::string& uriStr, bool isPredefined,
                                      bool& stubSelectionInvalidated)
{
    if (isPredefined)
    {
        const std::string deletedPath = angel_lsp::utils::IncludeResolver::NormalizePath(path);
        {
            std::lock_guard<std::mutex> configLock(m_runtimeConfigMutex);
            if (!m_effectivePredefined.empty() && PathsAreSameFile(deletedPath, m_effectivePredefined))
            {
                stubSelectionInvalidated = true;
            }
        }

        std::optional<std::string> owner = m_predefinedManager.GetUriByPath(path);
        if (!owner)
        {
            owner = m_predefinedManager.GetUriByPath(deletedPath);
        }
        if (!owner && !uriStr.empty() && m_predefinedManager.HasStub(uriStr))
        {
            owner = uriStr;
        }

        if (owner)
        {
            UnloadPredefinedUri(*owner);
            SetDefinedWordsFrom(path, {});
            SetDefinedWordsFrom(deletedPath, {});
            return true;
        }
        return false;
    }

    const std::string targetUri = !uriStr.empty() ? uriStr : UriFromPath(path);
    bool wasPublished = false;
    {
        std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
        wasPublished = m_publishedForModules.erase(targetUri) > 0;
    }
    const auto physicalUri = UriFromPath(path);
    PurgeClosureFile(physicalUri);
    if (targetUri != physicalUri)
    {
        PurgeClosureFile(targetUri);
    }
    if (wasPublished)
    {
        PublishDiagnostics(targetUri, {});
    }
    return GetIncludeGraph().RemoveFile(path);
}

void Server::ReindexTrackedClosureFile(const std::string& path, angel_lsp::parser::AngelScriptParser& parser)
{
    const auto uri = UriFromPath(path);
    std::string previous;
    {
        std::lock_guard lock(m_closureMutex);
        if (const auto indexed = m_indexedUriByPath.find(path); indexed != m_indexedUriByPath.end())
        {
            previous = indexed->second;
        }
        else if (m_closureDocuments.contains(uri))
        {
            previous = uri;
        }
    }
    if (previous.empty())
    {
        return;
    }

    PurgeClosureFile(previous);
    IndexClosureFile(path, parser);
    const auto claim = ClaimFor(path);
    if (claim.owner && !claim.owner->isImplicitEntryPoint)
    {
        ScheduleAnalysis(uri, ReadClosedDocumentContent(uri));
    }
}

bool Server::HandleWatchedFileChanged(const std::string& path, bool isPredefined,
                                      angel_lsp::parser::AngelScriptParser& parser)
{
    if (isPredefined)
    {
        if (m_config.features.enablePredefinedLoader)
        {
            GetWorkspaceStore().InvalidatePredefinedBaseTable();
            ParserPredefined(path, parser, true);
            SetPredefinedReady(true);
            return true;
        }
        return false;
    }

    LogInfo(fmt::format("[File Read] Reading modified watched file: {}", path));
    utils::HighResTimer readTimer;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    LogInfo(fmt::format("[File Read] Finished reading watched file: {} in {} ({} bytes)", path,
                        utils::FormatDuration(readTimer.ElapsedMs()), content.size()));
    GetIncludeGraph().UpdateFile(
        utils::WorkspaceIncludeGraph::UpdateFileRequest{path,
                                                        content,
                                                        *SearchDirectories(),
                                                        IncludeAllowedRoots(),
                                                        std::string(ImplicitIncludeExtension()),
                                                        {DefinedWords(), m_config.preprocessor}});

    ReindexTrackedClosureFile(path, parser);
    return true;
}

void Server::ReplaceDeletedPredefinedStub(angel_lsp::parser::AngelScriptParser& parser)
{
    std::string replacement;
    {
        std::lock_guard<std::mutex> configLock(m_runtimeConfigMutex);
        std::erase_if(m_discoveredPredefined, [this](const std::string& candidate)
                      { return PathsAreSameFile(candidate, m_effectivePredefined); });

        if (!m_discoveredPredefined.empty())
        {
            replacement = m_discoveredPredefined.front();
        }
        m_effectivePredefined = replacement;
    }

    if (!replacement.empty())
    {
        LogInfo(fmt::format("The predefined stub in force was deleted; using {} instead",
                            std::filesystem::path(replacement).filename().string()));
        ParserPredefined(replacement, parser);
    }
    else
    {
        LogInfo("The predefined stub in force was deleted, and no other was found");
    }
    GetWorkspaceStore().InvalidatePredefinedBaseTable();
    SetPredefinedReady(true);
}

void Server::RefreshOpenDocumentIncludes()
{
    for (const auto& [openUri, openText] : GetDocumentStore().GetSnapshot())
    {
        if (const std::string openPath = CanonicalPathFromUri(openUri); !openPath.empty())
        {
            GetIncludeGraph().UpdateFile(
                utils::WorkspaceIncludeGraph::UpdateFileRequest{openPath,
                                                                openText,
                                                                *SearchDirectories(),
                                                                IncludeAllowedRoots(),
                                                                std::string(ImplicitIncludeExtension()),
                                                                {DefinedWords(), m_config.preprocessor}});
        }
    }
}

void Server::HandleNotificationsWorkspace_DidChangeWatchedFiles(
    lsp::notifications::Workspace_DidChangeWatchedFiles::Params&& params)
{
    angel_lsp::parser::AngelScriptParser watchedParser(m_logger.get());
    bool graphChanged = false;
    bool stubSelectionInvalidated = false;
    const auto previousWords = DefinedWords();

    for (const auto& event : params.changes)
    {
        const std::string uriStr = DocumentKey(event.uri.toString());
        if (GetDocumentStore().IsOpen(uriStr))
        {
            continue;
        }

        const std::string path = CanonicalPathFromUri(uriStr);
        if (path.empty())
        {
            continue;
        }

        const bool isPredefined = angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension);
        const bool isRulesFile = path.ends_with("angelscript.rules.json") ||
                                 (!m_config.rulesFile.empty() && PathsAreSameFile(path, m_config.rulesFile));
        if (isRulesFile)
        {
            LoadWorkspaceEngineRules(WorkspaceRootPaths());
            graphChanged = true;
        }
        else if (event.type == lsp::FileChangeType::Deleted)
        {
            graphChanged =
                HandleWatchedFileDeleted(path, uriStr, isPredefined, stubSelectionInvalidated) || graphChanged;
        }
        else
        {
            graphChanged = HandleWatchedFileChanged(path, isPredefined, watchedParser) || graphChanged;
        }
    }

    if (stubSelectionInvalidated)
    {
        ReplaceDeletedPredefinedStub(watchedParser);
        graphChanged = true;
    }

    if (previousWords != DefinedWords())
    {
        RestartWorkspaceScan();
        return;
    }
    if (!graphChanged)
    {
        return;
    }

    RefreshOpenDocumentIncludes();
    ReanalyseOpenDocuments();
}

} // namespace angel_lsp
