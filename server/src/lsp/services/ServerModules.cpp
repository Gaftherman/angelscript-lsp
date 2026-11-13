#include "lsp/Server.h"
#include "lsp/services/ServerModuleDefinition.h"
#include "lsp/services/ServerModuleUtils.h"
#include "utils/IncludeResolver.h"

#include <algorithm>
#include <filesystem>
#include <spdlog/fmt/fmt.h>
#include <string_view>
#include <vector>

namespace angel_lsp
{
static void AppendUniqueDirectory(std::vector<std::string>& dirs, std::string_view dir, bool& added)
{
    if (dir.empty())
    {
        return;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(dir), ec) &&
        std::find(dirs.begin(), dirs.end(), dir) == dirs.end())
    {
        dirs.emplace_back(dir);
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

ModuleClaim Server::ClaimFor(std::string_view normalizedPath) const
{
    std::vector<ModuleCandidate> candidates;

    for (const auto& view : m_modules)
    {
        if (view.closurePaths.contains(std::string(normalizedPath)))
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
                                           {
                                               return a.rank < b.rank;
                                           }
                                           return a.depth < b.depth;
                                       });

    claim.owner = best->view;

    for (const auto& candidate : candidates)
    {
        if (candidate.view != claim.owner)
        {
            claim.alsoClaimedBy.push_back(candidate.view->name);
        }
    }

    return claim;
}

std::string Server::ResolveConfiguredPath(std::string_view configured) const
{
    std::string_view cleaned = configured;
    for (const std::string_view prefix :
         {"${workspaceFolder}/", "${workspaceFolder}\\", "${workspaceRoot}/", "${workspaceRoot}\\"})
    {
        if (cleaned.starts_with(prefix))
        {
            cleaned.remove_prefix(prefix.size());
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
} // namespace angel_lsp
