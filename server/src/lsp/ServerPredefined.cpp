#include "analysis/EngineProfiles.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include "utils/WorkspaceScan.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <spdlog/fmt/fmt.h>

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
    auto kind = angel_lsp::analysis::ParseEngineProfileKind(profileName);

    if (!profileName.empty() && !angel_lsp::analysis::IsKnownEngineProfileName(profileName))
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
    const std::string stubSource = angel_lsp::analysis::GetProfileStubText(pKind);
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
            for (const auto& workspaceRoot : WorkspaceRoots())
                candidates.push_back(std::filesystem::path(angel_lsp::utils::UriToPath(workspaceRoot)) / configured);
        }

        bool loaded = false;
        for (const auto& candidate : candidates)
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

bool Server::RefreshStubDefinedWords(const std::string& uriStr, const std::string& text)
{
    const std::string path = CanonicalPathFromUri(uriStr);
    if (path.empty())
        return false;

    if (!PredefinedStubContributes(uriStr))
        return false;

    return SetDefinedWordsFrom(path, angel_lsp::utils::ScanDefinedWords(text));
}

void Server::UnloadUnselectedPredefinedStubs(const std::vector<std::string>& wantedPaths)
{
    std::vector<std::string> stale;
    for (const auto& [path, uri] : m_predefinedManager.GetAllUriByPath())
    {
        const bool wanted = std::any_of(wantedPaths.begin(), wantedPaths.end(), [&path](const std::string& candidate)
                                        { return PathsAreSameFile(candidate, path); });
        bool isTransitive = false;
        {
            std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
            isTransitive = m_predefinedTransitiveIncludes.contains(path);
        }
        if (!wanted && !isTransitive)
            stale.push_back(uri);
    }

    for (const auto& uri : stale)
    {
        if (UnloadPredefinedUri(uri))
            LogInfo(fmt::format("Unloaded predefined stub that is no longer selected: {}", uri));
    }
}

bool Server::PathsAreSameFile(const std::string& a, const std::string& b)
{
    if (a == b)
    {
        return true;
    }

#if defined(_WIN32)
    if (a.size() == b.size() &&
        std::equal(
            a.begin(), a.end(), b.begin(), [](char x, char y)
            { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); }))
    {
        return true;
    }
#endif

    std::error_code ec;
    if (!a.empty() && !b.empty() && std::filesystem::equivalent(a, b, ec) && !ec)
    {
        return true;
    }

    return false;
}

void Server::ReportPredefinedSelection(const std::vector<std::string>& discovered, const std::string& activePath,
                                       const std::string& autoSelected, bool mergeAll)
{
    const auto tell = [this](lsp::MessageType type, const std::string& text)
    {
        lsp::notifications::Window_ShowMessage::Params params;
        params.type = type;
        params.message = text;
        m_messageHandler->sendNotification<lsp::notifications::Window_ShowMessage>(std::move(params));
    };

    if (!activePath.empty())
    {
        const bool found = std::any_of(discovered.begin(), discovered.end(), [&activePath](const std::string& path)
                                       { return PathsAreSameFile(path, activePath); });

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
            list += ", ";
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

bool Server::UnloadPredefinedUri(std::string uriStr)
{
    std::string path;
    if (!m_predefinedManager.UnloadUri(uriStr, &path))
    {
        return false;
    }

    m_symbolTable.ClearDocumentSymbols(uriStr);
    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);

    if (!path.empty())
    {
        SetDefinedWordsFrom(path, {});
    }

    return true;
}

bool Server::PredefinedStubContributes(const std::string& uriStr) const
{
    if (uriStr.starts_with(angel_lsp::analysis::k_profileUriPrefix))
    {
        return true;
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

bool Server::ClaimPredefinedFile(const std::string& uriStr, bool forceReload)
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

void Server::ParserPredefined(const std::string& filePath, angel_lsp::parser::AngelScriptParser& parser,
                              bool forceReload)
{
    std::unordered_set<std::string> visited;
    ParserPredefinedInternal(filePath, parser, forceReload, visited);
}

void Server::ParserPredefinedInternal(const std::string& filePath, angel_lsp::parser::AngelScriptParser& parser,
                                      bool forceReload, std::unordered_set<std::string>& visited)
{
    const std::string normPath = angel_lsp::utils::IncludeResolver::NormalizePath(filePath);
    if (!visited.insert(normPath).second)
    {
        return;
    }

    utils::HighResTimer totalTimer;
    std::string uri = UriFromPath(filePath);

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
    {
        LogError(fmt::format("Cannot open predefined file: {}", filePath));
        UnloadPredefinedUri(uri);
        return;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    content = angel_lsp::utils::SanitizePredefinedContent(content);

    if (!ClaimPredefinedFile(uri, forceReload))
    {
        return;
    }

    m_predefinedManager.SetDocumentText(uri, content);

    utils::HighResTimer parseTimer;
    document::TreePtr tree = document::MakeTreePtr(parser.Parse(content));
    int64_t parseUs = parseTimer.ElapsedUs();

    utils::HighResTimer symTimer;
    ReplaceSymbolsFromTree(uri, content, tree.get());
    int64_t symUs = symTimer.ElapsedUs();

    utils::HighResTimer scopeTimer;
    m_scopeIndex.ClearDocument(uri);
    m_callGraph.ClearDocument(uri);
    tree.reset();
    int64_t scopeUs = scopeTimer.ElapsedUs();

    if (SetDefinedWordsFrom(normPath, angel_lsp::utils::ScanDefinedWords(content)))
        LogInfo(fmt::format("Defined words changed after loading: {}", filePath));

    int64_t totalUs = totalTimer.ElapsedUs();
    LogInfo(
        fmt::format("[ParserPredefined Profile] File: {} | Total: {} us (Parse: {} us, Symbols: {} us, Scopes: {} us)",
                    filePath, totalUs, parseUs, symUs, scopeUs));
    LogInfo(fmt::format("Loaded predefined file: {}", filePath));

    PredefinedLoadContext ctx{parser, forceReload, visited};
    LoadPredefinedIncludes(content, filePath, ctx);
}

void Server::LoadPredefinedIncludes(const std::string& content, const std::string& filePath, PredefinedLoadContext& ctx)
{
    std::vector<std::string> searchDirs;
    if (const auto dirs = SearchDirectories())
    {
        searchDirs.insert(searchDirs.end(), dirs->begin(), dirs->end());
    }
    for (const auto& wsRoot : WorkspaceRoots())
    {
        if (std::string wsPath = angel_lsp::utils::UriToPath(wsRoot); !wsPath.empty())
        {
            searchDirs.push_back(std::move(wsPath));
        }
    }
    const auto allowedRoots = IncludeAllowedRoots();

    for (const auto& inc : angel_lsp::utils::IncludeResolver::ExtractIncludes(content))
    {
        std::string resolved =
            angel_lsp::utils::IncludeResolver::ResolveIncludePath(angel_lsp::utils::IncludeResolveRequest{
                .includePath = inc.rawPath,
                .currentFilePath = filePath,
                .searchDirectories = searchDirs,
                .allowedRoots = allowedRoots,
                .implicitExtension = ImplicitIncludeExtension(),
            });
        if (!resolved.empty())
        {
            const std::string normInc = angel_lsp::utils::IncludeResolver::NormalizePath(resolved);
            {
                std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
                m_predefinedTransitiveIncludes.insert(normInc);
            }
            ParserPredefinedInternal(normInc, ctx.parser, ctx.forceReload, ctx.visited);
        }
    }
}

void Server::SetPredefinedReady(bool ready)
{
    {
        std::lock_guard<std::mutex> lock(m_predefinedMutex);
        m_predefinedReady.store(ready, std::memory_order_release);
    }
    m_predefinedCv.notify_all();
}

bool Server::WaitForPredefinedReady(std::chrono::milliseconds timeout) const
{
    if (m_predefinedReady.load(std::memory_order_acquire))
    {
        return true;
    }
    std::unique_lock<std::mutex> lock(m_predefinedMutex);
    return m_predefinedCv.wait_for(lock, timeout, [this] { return m_predefinedReady.load(std::memory_order_acquire); });
}

bool Server::IsPredefinedReady() const noexcept
{
    return m_predefinedReady.load(std::memory_order_acquire);
}
} // namespace angel_lsp
