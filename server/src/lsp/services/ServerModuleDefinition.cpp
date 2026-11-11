#include "lsp/services/ServerModuleDefinition.h"
#include "lsp/Server.h"
#include "lsp/services/ServerModuleUtils.h"
#include "utils/IncludeResolver.h"
#include "utils/WorkspaceScan.h"

#include <algorithm>
#include <filesystem>
#include <spdlog/fmt/fmt.h>
#include <vector>

namespace angel_lsp
{
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
        const auto allowedRoots = server.IncludeAllowedRoots();
        const utils::IncludeTraversalRequest includeRequest{view.entryPath,
                                                            *searchDirs,
                                                            allowedRoots,
                                                            {},
                                                            server.ImplicitIncludeExtension(),
                                                            {server.DefinedWords(), server.GetConfig().preprocessor}};
        for (const auto& member : utils::IncludeResolver::ResolveAllIncludes(includeRequest))
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

std::optional<ModuleView> ResolveModuleDefinition(Server& server,
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
} // namespace angel_lsp
