#include "analysis/types/EngineProfiles.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Utils.h"

#include <algorithm>
#include <filesystem>
#include <span>
#include <spdlog/fmt/fmt.h>
#include <string_view>
#include <vector>

namespace angel_lsp
{
bool Server::RefreshStubDefinedWords(std::string_view uriStr, std::string_view text)
{
    const std::string path = CanonicalPathFromUri(uriStr);
    if (path.empty())
    {
        return false;
    }

    if (!PredefinedStubContributes(uriStr))
    {
        return false;
    }

    return SetDefinedWordsFrom(path, angel_lsp::utils::ScanDefinedWords(text));
}

void Server::UnloadUnselectedPredefinedStubs(std::span<const std::string> wantedPaths)
{
    std::vector<std::string> stale;
    for (const auto& [path, uri] : m_predefinedManager.GetAllUriByPath())
    {
        if (GetDocumentStore().IsOpen(uri))
        {
            continue;
        }
        auto matchesPath = [&path](std::string_view candidate) { return PathsAreSameFile(candidate, path); };
        const bool wanted = std::any_of(wantedPaths.begin(), wantedPaths.end(), matchesPath);
        bool isTransitive = false;
        {
            std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
            isTransitive = m_predefinedTransitiveIncludes.contains(path);
        }
        if (!wanted && !isTransitive)
        {
            stale.push_back(uri);
        }
    }

    for (const auto& uri : stale)
    {
        if (UnloadPredefinedUri(uri))
        {
            LogInfo(fmt::format("Unloaded predefined stub that is no longer selected: {}", uri));
        }
    }
}

bool Server::PathsAreSameFile(std::string_view a, std::string_view b)
{
    if (a == b)
    {
        return true;
    }

#if defined(_WIN32)
    auto charEqualIgnoreCase = [](char x, char y)
    { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); };
    if (a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), charEqualIgnoreCase))
    {
        return true;
    }
#endif

    std::error_code ec;
    if (!a.empty() && !b.empty() &&
        std::filesystem::equivalent(std::filesystem::path(a), std::filesystem::path(b), ec) && !ec)
    {
        return true;
    }

    return false;
}

void Server::ReportPredefinedSelection(std::span<const std::string> discovered, std::string_view activePath,
                                       std::string_view autoSelected, bool mergeAll)
{
    const auto tell = [this](lsp::MessageType type, std::string_view text)
    {
        lsp::notifications::Window_ShowMessage::Params params;
        params.type = type;
        params.message = text;
        m_messageHandler->sendNotification<lsp::notifications::Window_ShowMessage>(std::move(params));
    };

    if (!activePath.empty())
    {
        auto matchesActive = [&activePath](std::string_view path) { return PathsAreSameFile(path, activePath); };
        const bool found = std::any_of(discovered.begin(), discovered.end(), matchesActive);

        if (!found)
        {
            tell(lsp::MessageType::Error,
                 fmt::format("AngelScript: the selected predefined stub was not found in the "
                             "workspace: {}. No host types will resolve until it is corrected.",
                             m_config.activePredefined));
        }
        return;
    }

    if (discovered.size() <= 1)
    {
        return;
    }

    std::string list;
    for (const auto& path : discovered)
    {
        if (!list.empty())
        {
            list += ", ";
        }
        list += std::filesystem::path(path).filename().string();
    }

    if (mergeAll)
    {
        LogInfo(fmt::format("AngelScript: {} predefined stubs loaded together ({}). Declarations "
                            "they share will resolve more than once.",
                            discovered.size(), list));
        return;
    }

    LogInfo(fmt::format("AngelScript: using {} of {} predefined stubs found ({}). Set "
                        "angelscript.predefined.active to choose another, or to \"all\" to load "
                        "them together.",
                        std::filesystem::path(autoSelected).filename().string(), discovered.size(), list));
}

bool Server::UnloadPredefinedUri(std::string_view uriStr)
{
    std::string path;
    const bool unloaded = m_predefinedManager.UnloadUri(uriStr, &path);

    m_symbolTable.ClearDocumentSymbols(std::string(uriStr));
    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);

    if (!path.empty())
    {
        SetDefinedWordsFrom(path, {});
    }

    SetPredefinedReady(true);

    return unloaded;
}

bool Server::PredefinedStubContributes(std::string_view uriStr) const
{
    if (uriStr.starts_with(angel_lsp::analysis::k_profileUriPrefix))
    {
        return true;
    }

    if (m_config.activePredefined == "all")
    {
        return true;
    }

    if (!m_config.activeStubFiles.empty())
    {
        const std::string path = CanonicalPathFromUri(uriStr);
        for (const auto& active : m_config.activeStubFiles)
        {
            if ((!path.empty() && PathsAreSameFile(path, angel_lsp::utils::IncludeResolver::NormalizePath(active))) ||
                path.ends_with(active) || uriStr.ends_with(active))
            {
                return true;
            }
        }
        return false;
    }

    std::string effective;
    bool isTransitive = false;
    const std::string path = CanonicalPathFromUri(uriStr);

    {
        std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
        effective = m_effectivePredefined;
        isTransitive = m_predefinedTransitiveIncludes.contains(path);
    }

    if (isTransitive)
    {
        return true;
    }

    for (const auto& configured : m_config.predefinedFiles)
    {
        if (!path.empty() && PathsAreSameFile(path, angel_lsp::utils::IncludeResolver::NormalizePath(configured)))
        {
            return true;
        }
    }

    if (effective.empty())
    {
        return true;
    }

    return !path.empty() && PathsAreSameFile(path, effective);
}

bool Server::ClaimPredefinedFile(std::string_view uriStr, bool forceReload)
{
    const std::string path = CanonicalPathFromUri(uriStr);
    std::string previous;
    const bool claimed = m_predefinedManager.ClaimFile(uriStr, path, forceReload, &previous);

    if (!previous.empty() && previous != uriStr)
    {
        UnloadPredefinedUri(previous);
        LogInfo(fmt::format("Predefined file re-indexed under {} (was {})", uriStr, previous));
    }

    return claimed;
}
} // namespace angel_lsp
