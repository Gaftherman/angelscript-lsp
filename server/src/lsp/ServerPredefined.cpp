#include "lsp/Server.h"
#include "analysis/EngineProfiles.h"
#include "utils/Utils.h"
#include "utils/Timer.h"
#include "utils/WorkspaceScan.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include <spdlog/fmt/fmt.h>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace angel_lsp
{
    void Server::LoadBuiltinEngineProfiles(angel_lsp::parser::AngelScriptParser &parser,
                                           const angel_lsp::utils::StopFlag &stopToken)
    {
        if (!m_config.features.enablePredefinedLoader)
        {
            return;
        }

        const std::string profileName = EngineProfile();
        auto kind = angel_lsp::analysis::ParseEngineProfileKind(profileName);

        if (!profileName.empty() && !angel_lsp::analysis::IsKnownEngineProfileName(profileName))
        {
            lsp::notifications::Window_ShowMessage::Params params;
            params.type = lsp::MessageType::Warning;
            params.message = fmt::format(
                "AngelScript: '{}' is not an engine profile this server knows, so the standard "
                "profile was loaded instead. Known names: standard, svencoop, urho3d, openxray, "
                "ootp, auto, none.",
                profileName);
            m_messageHandler->sendNotification<lsp::notifications::Window_ShowMessage>(std::move(params));
        }

        if (kind == angel_lsp::analysis::EngineProfileKind::None)
        {
            return;
        }

        if (kind == angel_lsp::analysis::EngineProfileKind::Auto)
        {
            std::vector<std::string> rootPaths;
            for (const auto &workspaceRoot : WorkspaceRoots())
                rootPaths.push_back(angel_lsp::utils::UriToPath(workspaceRoot));

            std::vector<std::string> fileNames;
            const bool completed = angel_lsp::utils::ForEachWorkspaceFile(
                rootPaths, m_config.exclude,
                [&stopToken]() { return stopToken.stop_requested(); },
                [&fileNames](const std::filesystem::directory_entry &entry) {
                    fileNames.push_back(entry.path().filename().string());
                });

            if (!completed)
                return;
            kind = angel_lsp::analysis::DetectEngineProfileFromWorkspace(fileNames);
        }

        std::vector<angel_lsp::analysis::EngineProfileKind> profilesToLoad;
        if (kind != angel_lsp::analysis::EngineProfileKind::Standard &&
            kind != angel_lsp::analysis::EngineProfileKind::None)
        {
            profilesToLoad.push_back(angel_lsp::analysis::EngineProfileKind::Standard);
        }
        profilesToLoad.push_back(kind);

        {
            std::vector<std::string> wanted;
            wanted.reserve(profilesToLoad.size());
            for (const auto pKind : profilesToLoad)
                wanted.push_back(angel_lsp::analysis::GetProfileSyntheticUri(pKind));

            std::lock_guard<std::mutex> lock(m_predefinedMutex);

            std::vector<std::string> stale;
            for (const auto &loaded : m_predefinedUris)
            {
                if (!loaded.starts_with(angel_lsp::analysis::k_profileUriPrefix))
                    continue;
                if (std::find(wanted.begin(), wanted.end(), loaded) == wanted.end())
                    stale.push_back(loaded);
            }

            for (const auto &uri : stale)
            {
                UnloadPredefinedUri(uri);
                LogInfo(fmt::format("Unloaded built-in engine profile: {}", uri));
            }
        }

        for (auto pKind : profilesToLoad)
        {
            if (stopToken.stop_requested())
            {
                return;
            }

            const std::string stubSource = angel_lsp::analysis::GetProfileStubText(pKind);
            if (stubSource.empty())
            {
                continue;
            }

            const std::string syntheticUri = angel_lsp::analysis::GetProfileSyntheticUri(pKind);

            std::lock_guard<std::mutex> lock(m_predefinedMutex);
            if (!ClaimPredefinedFile(syntheticUri, /*forceReload=*/false))
            {
                continue;
            }

            ReplaceSymbolsFromSource(syntheticUri, stubSource, parser);

            m_scopeIndex.ClearDocument(syntheticUri);
            m_callGraph.ClearDocument(syntheticUri);
            m_scopeIndex.SetScopeTree(syntheticUri, m_localScopeCollector->CollectScopes(std::string(stubSource), parser));

            LogInfo(fmt::format("Loaded built-in engine profile: {}", angel_lsp::analysis::EngineProfileKindToString(pKind)));
        }
    }

    std::vector<std::string> Server::LoadConfiguredPredefinedFiles(angel_lsp::parser::AngelScriptParser &parser,
                                                                   const angel_lsp::utils::StopFlag &stopToken)
    {
        std::vector<std::string> loadedPaths;

        for (const auto &entry : m_config.predefinedFiles)
        {
            if (stopToken.stop_requested())
                return loadedPaths;

            std::error_code ec;
            const std::filesystem::path configured(entry);

            std::vector<std::filesystem::path> candidates;
            if (configured.is_absolute())
            {
                candidates.push_back(configured);
            }
            else
            {
                for (const auto &workspaceRoot : WorkspaceRoots())
                    candidates.push_back(std::filesystem::path(angel_lsp::utils::UriToPath(workspaceRoot)) / configured);
            }

            bool loaded = false;
            for (const auto &candidate : candidates)
            {
                if (!std::filesystem::is_regular_file(candidate, ec))
                    continue;

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

    bool Server::RefreshStubDefinedWords(const std::string &uriStr, const std::string &text)
    {
        const std::string path = CanonicalPathFromUri(uriStr);
        if (path.empty())
            return false;

        {
            std::lock_guard<std::mutex> lock(m_predefinedMutex);
            if (!PredefinedStubContributes(uriStr))
                return false;
        }

        return SetDefinedWordsFrom(path, angel_lsp::utils::ScanDefinedWords(text));
    }

    void Server::UnloadUnselectedPredefinedStubs(const std::vector<std::string> &wantedPaths)
    {
        std::lock_guard<std::mutex> lock(m_predefinedMutex);

        std::vector<std::string> stale;
        for (const auto &[path, uri] : m_predefinedUriByPath)
        {
            const bool wanted = std::any_of(wantedPaths.begin(), wantedPaths.end(),
                                            [&path](const std::string &candidate)
                                            { return PathsAreSameFile(candidate, path); });
            if (!wanted)
                stale.push_back(uri);
        }

        for (const auto &uri : stale)
        {
            if (UnloadPredefinedUri(uri))
                LogInfo(fmt::format("Unloaded predefined stub that is no longer selected: {}", uri));
        }
    }

    bool Server::PathsAreSameFile(const std::string &a, const std::string &b)
    {
#if defined(_WIN32)
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
#else
        return a == b;
#endif
    }

    void Server::ReportPredefinedSelection(const std::vector<std::string> &discovered,
                                           const std::string &activePath,
                                           const std::string &autoSelected,
                                           bool mergeAll)
    {
        const auto tell = [this](lsp::MessageType type, const std::string &text) {
            lsp::notifications::Window_ShowMessage::Params params;
            params.type = type;
            params.message = text;
            m_messageHandler->sendNotification<lsp::notifications::Window_ShowMessage>(std::move(params));
        };

        if (!activePath.empty())
        {
            const bool found = std::any_of(discovered.begin(), discovered.end(),
                                           [&activePath](const std::string &path) {
                                               return PathsAreSameFile(path, activePath);
                                           });

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
        for (const auto &path : discovered)
        {
            if (!list.empty())
                list += ", ";
            list += std::filesystem::path(path).filename().string();
        }

        if (mergeAll)
        {
            LogInfo(
                fmt::format("AngelScript: {} predefined stubs loaded together ({}). Declarations "
                            "they share will resolve more than once.",
                            discovered.size(), list));
            return;
        }

        LogInfo(
            fmt::format("AngelScript: using {} of {} predefined stubs found ({}). Set "
                        "angelscript.predefined.active to choose another, or to \"all\" to load "
                        "them together.",
                        std::filesystem::path(autoSelected).filename().string(),
                        discovered.size(), list));
    }

    bool Server::UnloadPredefinedUri(std::string uriStr)
    {
        if (!m_predefinedUris.contains(uriStr))
        {
            return false;
        }

        m_symbolTable.ClearDocumentSymbols(uriStr);
        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);
        m_predefinedUris.erase(uriStr);
        m_predefinedDocuments.erase(uriStr);

        for (auto it = m_predefinedUriByPath.begin(); it != m_predefinedUriByPath.end(); ++it)
        {
            if (it->second == uriStr)
            {
                SetDefinedWordsFrom(it->first, {});
                m_predefinedUriByPath.erase(it);
                break;
            }
        }

        return true;
    }

    bool Server::PredefinedStubContributes(const std::string &uriStr) const
    {
        std::string effective;
        {
            std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
            effective = m_effectivePredefined;
        }

        if (effective.empty())
        {
            return true;
        }

        if (m_predefinedUris.contains(uriStr))
        {
            return true;
        }

        const std::string path = CanonicalPathFromUri(uriStr);
        return !path.empty() && PathsAreSameFile(path, effective);
    }

    bool Server::ClaimPredefinedFile(const std::string &uriStr, bool forceReload)
    {
        const std::string path = CanonicalPathFromUri(uriStr);

        if (path.empty())
        {
            return m_predefinedUris.insert(uriStr).second || forceReload;
        }

        if (const auto owner = m_predefinedUriByPath.find(path); owner != m_predefinedUriByPath.end())
        {
            if (owner->second == uriStr)
            {
                return forceReload;
            }

            const std::string previous = owner->second;
            UnloadPredefinedUri(previous);

            LogInfo(fmt::format("Predefined file re-indexed under {} (was {})", uriStr, previous));
        }

        m_predefinedUriByPath[path] = uriStr;
        m_predefinedUris.insert(uriStr);
        return true;
    }

    void Server::ParserPredefined(const std::string &filePath, angel_lsp::parser::AngelScriptParser &parser, bool forceReload)
    {
        utils::HighResTimer totalTimer;
        std::string uri = UriFromPath(filePath);

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            LogError(fmt::format("Cannot open predefined file: {}", filePath));
            return;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        content = angel_lsp::utils::SanitizePredefinedContent(content);

        std::lock_guard<std::mutex> lock(m_predefinedMutex);

        if (!ClaimPredefinedFile(uri, forceReload))
        {
            return;
        }

        m_predefinedDocuments[uri] = content;

        utils::HighResTimer parseTimer;
        document::TreePtr tree = document::MakeTreePtr(parser.Parse(content));
        int64_t parseUs = parseTimer.ElapsedUs();

        utils::HighResTimer symTimer;
        ReplaceSymbolsFromTree(uri, content, tree.get());
        int64_t symUs = symTimer.ElapsedUs();

        utils::HighResTimer scopeTimer;
        m_scopeIndex.ClearDocument(uri);
        m_callGraph.ClearDocument(uri);
        if (tree)
        {
            m_scopeIndex.SetScopeTree(uri,
                m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(tree.get()), content));
        }
        tree.reset();
        int64_t scopeUs = scopeTimer.ElapsedUs();

        if (SetDefinedWordsFrom(angel_lsp::utils::IncludeResolver::NormalizePath(filePath),
                                angel_lsp::utils::ScanDefinedWords(content)))
            LogInfo(fmt::format("Defined words changed after loading: {}", filePath));

        int64_t totalUs = totalTimer.ElapsedUs();
        LogInfo(fmt::format(
            "[ParserPredefined Profile] File: {} | Total: {} us (Parse: {} us, Symbols: {} us, Scopes: {} us)",
            filePath, totalUs, parseUs, symUs, scopeUs));
        LogInfo(fmt::format("Loaded predefined file: {}", filePath));
    }
}
