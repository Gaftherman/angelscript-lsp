#include "analysis/types/EngineProfiles.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include "utils/Utils.h"
#include "utils/WorkspaceScan.h"

#include <algorithm>
#include <filesystem>
#include <spdlog/fmt/fmt.h>
#include <vector>

namespace angel_lsp
{
std::optional<angel_lsp::analysis::EngineProfileKind>
Server::ResolveTargetEngineProfile(const angel_lsp::utils::StopFlag& stopToken)
{
    if (!m_config.features.enablePredefinedLoader)
    {
        return std::nullopt;
    }

    const std::string profileName = EngineProfile();
    if (profileName.empty() || profileName == "none")
    {
        return std::nullopt;
    }

    LogWarning(fmt::format("Built-in engine profiles are deprecated. Profile '{}' was requested, but predefined stub "
                           "files (.as.predefined) are recommended.",
                           profileName));

    auto kind = angel_lsp::analysis::ParseEngineProfileKind(profileName);

    if (!angel_lsp::analysis::IsKnownEngineProfileName(profileName))
    {
        lsp::notifications::Window_ShowMessage::Params params;
        params.type = lsp::MessageType::Warning;
        params.message = fmt::format("AngelScript: '{}' is not an engine profile this server knows, so the standard "
                                     "profile was loaded instead. Known names: standard, svencoop, urho3d, openxray, "
                                     "ootp, auto, none.",
                                     profileName);
        m_messageHandler->sendNotification<lsp::notifications::Window_ShowMessage>(std::move(params));
    }

    if (kind == angel_lsp::analysis::EngineProfileKind::None)
    {
        return std::nullopt;
    }

    if (kind == angel_lsp::analysis::EngineProfileKind::Auto)
    {
        std::vector<std::string> rootPaths;
        for (const auto& workspaceRoot : WorkspaceRoots())
        {
            rootPaths.push_back(angel_lsp::utils::UriToPath(workspaceRoot));
        }

        std::vector<std::string> fileNames;
        const bool completed = angel_lsp::utils::ForEachWorkspaceFile(
            rootPaths, m_config.exclude, [&stopToken]() { return stopToken.stop_requested(); },
            [&fileNames](const std::filesystem::directory_entry& entry)
            { fileNames.push_back(entry.path().filename().string()); });

        if (!completed)
        {
            return std::nullopt;
        }
        kind = angel_lsp::analysis::DetectEngineProfileFromWorkspace(fileNames);
    }

    return kind;
}

void Server::UnloadStaleBuiltinEngineProfiles(const std::vector<angel_lsp::analysis::EngineProfileKind>& wantedProfiles)
{
    std::vector<std::string> wanted;
    wanted.reserve(wantedProfiles.size());
    for (const auto pKind : wantedProfiles)
    {
        wanted.push_back(angel_lsp::analysis::GetProfileSyntheticUri(pKind));
    }

    std::vector<std::string> stale;
    for (const auto& loaded : m_predefinedManager.GetLoadedUris())
    {
        if (!loaded.starts_with(angel_lsp::analysis::k_profileUriPrefix))
        {
            continue;
        }
        if (std::find(wanted.begin(), wanted.end(), loaded) == wanted.end())
        {
            stale.push_back(loaded);
        }
    }

    for (const auto& uri : stale)
    {
        UnloadPredefinedUri(uri);
        LogInfo(fmt::format("Unloaded built-in engine profile: {}", uri));
    }
}

void Server::LoadEngineProfileStub(angel_lsp::analysis::EngineProfileKind pKind,
                                   angel_lsp::parser::AngelScriptParser& parser)
{
    const std::string stubSource(angel_lsp::analysis::GetProfileStubText(pKind));
    if (stubSource.empty())
    {
        return;
    }

    const std::string syntheticUri = angel_lsp::analysis::GetProfileSyntheticUri(pKind);

    if (!ClaimPredefinedFile(syntheticUri, false))
    {
        return;
    }

    m_predefinedManager.SetDocumentText(syntheticUri, stubSource);

    ReplaceSymbolsFromSource(syntheticUri, stubSource, parser);

    m_scopeIndex.ClearDocument(syntheticUri);
    m_callGraph.ClearDocument(syntheticUri);

    LogInfo(fmt::format("Loaded built-in engine profile: {}", angel_lsp::analysis::EngineProfileKindToString(pKind)));
}

void Server::LoadBuiltinEngineProfiles(angel_lsp::parser::AngelScriptParser& parser,
                                       const angel_lsp::utils::StopFlag& stopToken)
{
    const auto targetKind = ResolveTargetEngineProfile(stopToken);
    if (!targetKind.has_value() || *targetKind == angel_lsp::analysis::EngineProfileKind::None)
    {
        UnloadStaleBuiltinEngineProfiles({});
        return;
    }

    std::vector<angel_lsp::analysis::EngineProfileKind> profilesToLoad;
    if (*targetKind != angel_lsp::analysis::EngineProfileKind::Standard &&
        *targetKind != angel_lsp::analysis::EngineProfileKind::None)
    {
        profilesToLoad.push_back(angel_lsp::analysis::EngineProfileKind::Standard);
    }
    profilesToLoad.push_back(*targetKind);

    UnloadStaleBuiltinEngineProfiles(profilesToLoad);

    for (auto pKind : profilesToLoad)
    {
        if (stopToken.stop_requested())
        {
            return;
        }

        LoadEngineProfileStub(pKind, parser);
    }
}

std::vector<std::string> Server::LoadConfiguredPredefinedFiles(angel_lsp::parser::AngelScriptParser& parser,
                                                               const angel_lsp::utils::StopFlag& stopToken)
{
    std::vector<std::string> loadedPaths;

    for (const auto& entry : m_config.predefinedFiles)
    {
        if (stopToken.stop_requested())
        {
            return loadedPaths;
        }

        std::error_code ec;
        const std::filesystem::path configured(entry);

        std::vector<std::filesystem::path> candidates;
        if (configured.is_absolute())
        {
            candidates.push_back(configured);
        }
        else
        {
            for (const auto& workspaceRoot : WorkspaceRoots())
            {
                candidates.push_back(std::filesystem::path(angel_lsp::utils::UriToPath(workspaceRoot)) / configured);
            }
        }

        bool loaded = false;
        for (const auto& candidate : candidates)
        {
            if (!std::filesystem::is_regular_file(candidate, ec))
            {
                continue;
            }

            ParserPredefined(candidate.string(), parser);
            loadedPaths.push_back(angel_lsp::utils::IncludeResolver::NormalizePath(candidate));
            loaded = true;
            break;
        }

        if (!loaded)
        {
            LogError(fmt::format("Configured predefined file not found: {}", entry));
        }
    }

    return loadedPaths;
}
} // namespace angel_lsp
