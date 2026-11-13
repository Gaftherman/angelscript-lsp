#include "lsp/Server.h"
#include "lsp/services/ServerModuleUtils.h"
#include "utils/IncludeResolver.h"
#include "utils/Utils.h"

#include <algorithm>
#include <filesystem>
#include <span>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp
{
static void AppendFilteredMembers(const ankerl::unordered_dense::set<std::string>& members, std::string_view openPath,
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

static void AppendEnclosingFolderModuleMembers(std::span<const ModuleView> modules, std::string_view openPath,
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

static void AppendIncludeGraphClosure(const Server& server, std::string_view openPath,
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
    const auto mc = server.GetIncludeGraph().GetModuleClosure(std::string(openPath));
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

std::vector<std::string> Server::ComputeModuleClosure(std::string_view openPath) const
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

void Server::WithdrawClosedSnapshot(std::string_view uriStr)
{
    std::lock_guard lock(m_closureMutex);
    m_closureDocuments.erase(std::string(uriStr));
}

void Server::PurgeClosureFile(std::string_view uriStr)
{
    std::lock_guard closureLock(m_closureMutex);
    const std::string uriCopy(uriStr);
    m_closureDocuments.erase(uriCopy);
    {
        std::lock_guard<std::mutex> lock(m_diagnosticsPipeline->GetCacheMutexRef());
        m_diagnosticsPipeline->GetCacheRef().erase(uriCopy);
    }

    m_symbolTable.ClearDocumentSymbols(uriCopy);
    m_scopeIndex.ClearDocument(uriCopy);
    m_callGraph.ClearDocument(uriCopy);

    const std::string path = CanonicalPathFromUri(uriCopy);
    if (const auto it = m_indexedUriByPath.find(path); it != m_indexedUriByPath.end() && it->second == uriCopy)
    {
        m_indexedUriByPath.erase(it);
    }
}

void Server::ReleaseModuleClosure(std::string_view openUriStr)
{
    std::vector<std::string> released;
    std::vector<std::string> toPurge;
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        const auto closure = m_openDocumentClosures.find(std::string(openUriStr));
        if (closure == m_openDocumentClosures.end())
        {
            return;
        }

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
