#include "lsp/Server.h"
#include "parser/AngelScriptParser.h"
#include "utils/IncludeResolver.h"
#include "utils/MultiFileLogger.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include "utils/WorkspaceScan.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace angel_lsp
{

bool Server::CollectWorkspaceFiles(const std::vector<std::string>& roots, const angel_lsp::utils::StopFlag& stopToken,
                                   WorkspaceFilesWalkResult& outFiles)
{
    PhaseTimer walkTimer(m_logger.get(), "unified workspace walk");
    return angel_lsp::utils::ForEachWorkspaceFile(
        roots, m_config.exclude, [&stopToken]() { return stopToken.stop_requested(); },
        [&](const std::filesystem::directory_entry& entry)
        {
            const std::string pathStr = entry.path().string();
            outFiles.allFileNames.push_back(entry.path().filename().string());

            if (angel_lsp::utils::IsPredefinedFile(pathStr, m_config.info.predefinedFileExtension))
            {
                outFiles.discoveredStubPaths.push_back(
                    angel_lsp::utils::IncludeResolver::NormalizeWalkedPath(entry.path()));
            }
            else if (!m_config.info.fileExtension.empty() &&
                     std::string_view(pathStr).ends_with(m_config.info.fileExtension))
            {
                outFiles.allScriptFiles.push_back(angel_lsp::utils::IncludeResolver::NormalizeWalkedPath(entry.path()));
            }
        });
}

void Server::ProcessDiscoveredPredefinedStubs(const std::vector<std::string>& discoveredStubPaths,
                                              const std::vector<std::string>& configuredPaths,
                                              const angel_lsp::utils::StopFlag& stopToken,
                                              angel_lsp::parser::AngelScriptParser& parser)
{
    try
    {
        PreScanStubDefinedWords(discoveredStubPaths);
        const bool mergeAll = (m_config.activePredefined == "all");
        const std::string activePath = (m_config.activePredefined.empty() || mergeAll)
                                           ? std::string()
                                           : ResolveConfiguredPath(m_config.activePredefined);

        std::vector<std::string> discovered;
        std::vector<std::string> wantedPaths = configuredPaths;

        for (const std::string& path : discoveredStubPaths)
        {
            if (stopToken.stop_requested())
            {
                return;
            }

            discovered.push_back(path);

            if (!activePath.empty())
            {
                if (PathsAreSameFile(path, activePath))
                {
                    ParserPredefined(path, parser);
                    wantedPaths.push_back(path);
                }
                continue;
            }

            if (mergeAll)
            {
                ParserPredefined(path, parser);
                wantedPaths.push_back(path);
            }
        }

        std::sort(discovered.begin(), discovered.end());

        std::string autoSelected;
        if (activePath.empty() && !mergeAll && !discovered.empty())
        {
            autoSelected = discovered.front();
            ParserPredefined(autoSelected, parser);
            wantedPaths.push_back(autoSelected);
        }

        {
            std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
            m_discoveredPredefined = discovered;
            m_effectivePredefined = autoSelected.empty() ? activePath : autoSelected;
        }

        UnloadUnselectedPredefinedStubs(wantedPaths);
        ReportPredefinedSelection(discovered, activePath, autoSelected, mergeAll);
    }
    catch (const std::exception& e)
    {
        LogError(fmt::format("Error reading workspace files: {}", e.what()));
    }
}

void Server::BuildIncludeGraphAndModules(const std::vector<std::string>& allScriptFiles,
                                         const std::vector<std::string>& roots,
                                         const angel_lsp::utils::StopFlag& stopToken,
                                         angel_lsp::parser::AngelScriptParser& parser)
{
    AugmentSearchDirectoriesFromModules();
    const auto searchDirectories = SearchDirectories();
    std::optional<PhaseTimer> phase;
    phase.emplace(m_logger.get(), "include graph");

    GetIncludeGraph().BuildFromFiles(utils::WorkspaceIncludeGraph::BuildFromFilesRequest{
        .scriptFiles = allScriptFiles,
        .searchDirectories = *searchDirectories,
        .workspaceRoots = roots,
        .shouldStop = [&stopToken]() { return stopToken.stop_requested(); },
        .fileReader = {},
        .implicitExtension = std::string(ImplicitIncludeExtension()),
        .allowedRoots = IncludeAllowedRoots(),
        .preprocessor = {DefinedWords(), m_config.preprocessor},
    });

    if (stopToken.stop_requested())
    {
        return;
    }

    LogInfo(fmt::format("Include graph built: {} script file(s)", GetIncludeGraph().FileCount()));
    ReportWorkspaceProgress(fmt::format("Indexed {} script file(s)", GetIncludeGraph().FileCount()), 60);

    phase.emplace(m_logger.get(), "configured modules");
    BuildModuleIndex();
    PurgeUnusedClosureFiles();
    IndexConfiguredModules(parser);

    phase.emplace(m_logger.get(), "rule index construction");
    m_symbolTable.EnsureRuleIndex();
    SyncModuleIndexSymbols();
    SetPredefinedReady(true);
    m_sessionCoordinator->IncrementConfigRevision();

    WithdrawStaleModuleDiagnostics();
    AnalyzeConfiguredModules();
}

bool Server::LoadAndProcessPredefinedStubs(const std::vector<std::string>& discoveredStubPaths,
                                           const angel_lsp::utils::StopFlag& stopToken,
                                           angel_lsp::parser::AngelScriptParser& parser)
{
    ReportWorkspaceProgress("Loading engine profiles", 15);
    {
        PhaseTimer profileTimer(m_logger.get(), "built-in engine profiles");
        LoadBuiltinEngineProfiles(parser, stopToken);
    }
    if (stopToken.stop_requested())
    {
        return false;
    }

    ReportWorkspaceProgress("Loading predefined stubs", 30);
    std::vector<std::string> configuredPaths;
    {
        PhaseTimer stubTimer(m_logger.get(), "configured predefined stubs");
        configuredPaths = LoadConfiguredPredefinedFiles(parser, stopToken);
    }

    {
        PhaseTimer discoveryTimer(m_logger.get(), "workspace stub discovery");
        ProcessDiscoveredPredefinedStubs(discoveredStubPaths, configuredPaths, stopToken, parser);
    }
    return !stopToken.stop_requested();
}

void Server::RecoverFromScanFailure(std::string_view err)
{
    LogError(err);
    angel_lsp::utils::MultiFileLogger::Instance().LogCrash(err);
    SetPredefinedReady(true);
    m_sessionCoordinator->SetWorkspaceScanComplete(true);
    EndWorkspaceProgress("Failed");
}

void Server::ReadWorkspaceFiles(const angel_lsp::utils::StopFlag& stopToken)
{
    try
    {
        const std::vector<std::string> roots = WorkspaceRootPaths();
        angel_lsp::utils::IncludeResolver::ForgetCanonicalDirectories();
        BeginWorkspaceProgress("AngelScript: indexing workspace");
        ReportWorkspaceProgress("Building the include graph", 0);

        const PhaseTimer scanTimer(m_logger.get(), "workspace scan (total)");
        ReanalyseOnExit reanalyseOnExit{[this, &stopToken]()
                                        {
                                            if (!IsPredefinedReady())
                                            {
                                                SetPredefinedReady(true);
                                            }
                                            SetWorkspaceScanComplete(true);
                                            if (!stopToken.stop_requested())
                                            {
                                                ScheduleOpenDocumentsForReanalysis();
                                            }
                                        }};

        WorkspaceFilesWalkResult walkResult;
        if (!CollectWorkspaceFiles(roots, stopToken, walkResult))
        {
            EndWorkspaceProgress("Cancelled");
            return;
        }

        angel_lsp::parser::AngelScriptParser backgroundParser(m_logger.get());
        if (m_config.features.enablePredefinedLoader &&
            !LoadAndProcessPredefinedStubs(walkResult.discoveredStubPaths, stopToken, backgroundParser))
        {
            EndWorkspaceProgress("Cancelled");
            return;
        }
        LoadWorkspaceEngineRules(roots);
        BuildIncludeGraphAndModules(walkResult.allScriptFiles, roots, stopToken, backgroundParser);
        if (stopToken.stop_requested())
        {
            EndWorkspaceProgress("Cancelled");
            return;
        }

        EndWorkspaceProgress(fmt::format("{} script file(s) indexed", GetIncludeGraph().FileCount()));
    }
    catch (const std::exception& e)
    {
        RecoverFromScanFailure(fmt::format("Fatal error in workspace scan thread: {}", e.what()));
    }
    catch (...)
    {
        RecoverFromScanFailure("Unknown fatal error in workspace scan thread.");
    }
}

void Server::StartWorkspaceScan()
{
    m_workspaceStop.Request();
    if (m_workspaceThread.joinable())
        m_workspaceThread.join();

    m_workspaceStop.Clear();
    m_workspaceThread = std::thread([this] { this->ReadWorkspaceFiles(m_workspaceStop); });
}

void Server::RestartWorkspaceScan()
{
    // The scan in flight is stopped and joined before the new one begins, so the two never
    // read the workspace state at once. Run on the workspace thread for the same reason it is
    // at startup: a full scan must not block the message loop.
    StartWorkspaceScan();
}

} // namespace angel_lsp
