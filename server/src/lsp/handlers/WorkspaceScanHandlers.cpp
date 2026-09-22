#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include "utils/WorkspaceScan.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
namespace
{
/**
 * @brief Logs how long a startup phase took, when it ends.
 *
 * The workspace scan reports progress as percentages that were written into the source by
 * hand: "55" for the engine profiles, "70" for the stubs. Those numbers describe the order
 * of the phases and nothing about their cost, so a report of "2942 ms to load on a small
 * project" had nowhere to be looked up. This gives every phase a number that came from a
 * clock.
 *
 * Info level, so it is in the log a user is already asked to attach to a report, and off
 * the wire otherwise.
 */
class PhaseTimer
{
  public:
    PhaseTimer(angel_lsp::utils::LspLogger* logger, std::string phase)
        : m_logger(logger), m_phase(std::move(phase)), m_start(std::chrono::steady_clock::now())
    {
    }

    ~PhaseTimer()
    {
        if (m_logger == nullptr)
        {
            return;
        }

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start);
        m_logger->LogInfo(fmt::format("Startup phase '{}': {} ms", m_phase, elapsed.count()));
    }

    PhaseTimer(const PhaseTimer&) = delete;
    PhaseTimer& operator=(const PhaseTimer&) = delete;

  private:
    angel_lsp::utils::LspLogger* m_logger;
    std::string m_phase;
    std::chrono::steady_clock::time_point m_start;
};

/**
 * @brief Constructs a TextEdit for replacing an include directive's path substring.
 * @param[in] line The source text line containing the directive.
 * @param[in] directive The extracted include directive metadata.
 * @param[in] replacement The new relative path replacement string.
 * @return TextEdit if opening and closing delimiters are matched, or std::nullopt otherwise.
 */
std::optional<lsp::TextEdit> TryBuildDirectiveEdit(std::string_view line,
                                                   const angel_lsp::utils::IncludeDirective& directive,
                                                   std::string_view replacement)
{
    const char open = directive.isAngled ? '<' : '"';
    const char close = directive.isAngled ? '>' : '"';
    const size_t openPos = line.find(open);
    if (openPos == std::string_view::npos)
    {
        return std::nullopt;
    }
    const size_t closePos = line.find(close, openPos + 1);
    if (closePos == std::string_view::npos)
    {
        return std::nullopt;
    }

    lsp::TextEdit edit;
    edit.range.start.line = static_cast<uint32_t>(directive.line);
    edit.range.start.character = static_cast<uint32_t>(openPos + 1);
    edit.range.end.line = static_cast<uint32_t>(directive.line);
    edit.range.end.character = static_cast<uint32_t>(closePos);
    edit.newText = std::string(replacement);
    return edit;
}
} // namespace

void Server::BeginWorkspaceProgress(const std::string& title)
{
    if (!m_workDoneProgressSupport)
    {
        return;
    }

    // A fresh token per scan. RestartWorkspaceScan can begin a second scan after a folder
    // change, and reusing a token would have the client fold the two into one bar that never
    // ends.
    m_workspaceProgressToken = "angelscript-workspace-scan-" + std::to_string(++m_workspaceProgressCounter);

    lsp::WorkDoneProgressCreateParams createParams;
    createParams.token = m_workspaceProgressToken;
    // Fire and forget: the client either makes room for the token or it does not, and either
    // way the scan carries on. Waiting on the response here would block the thread doing the
    // work for no benefit.
    m_messageHandler->sendRequest<lsp::requests::Window_WorkDoneProgress_Create>(
        std::move(createParams), [](auto&&) {}, [](const auto&) {});

    lsp::WorkDoneProgressBegin begin;
    begin.title = title;
    // Cancellable now that window/workDoneProgress/cancel is answered. Announcing it while
    // nothing handled the notification would have shown the user a cancel button that did
    // nothing, which is worse than no button.
    begin.cancellable = true;
    begin.percentage = 0u;

    lsp::notifications::Progress::Params params;
    params.token = m_workspaceProgressToken;
    params.value = lsp::toJson(std::move(begin));
    m_messageHandler->sendNotification<lsp::notifications::Progress>(std::move(params));
}

void Server::ReportWorkspaceProgress(const std::string& message, unsigned percentage)
{
    if (!m_workDoneProgressSupport || m_workspaceProgressToken.empty())
    {
        return;
    }

    lsp::WorkDoneProgressReport report;
    report.message = message;
    report.percentage = percentage;

    lsp::notifications::Progress::Params params;
    params.token = m_workspaceProgressToken;
    params.value = lsp::toJson(std::move(report));
    m_messageHandler->sendNotification<lsp::notifications::Progress>(std::move(params));
}

void Server::EndWorkspaceProgress(const std::string& message)
{
    if (!m_workDoneProgressSupport || m_workspaceProgressToken.empty())
    {
        return;
    }

    lsp::WorkDoneProgressEnd end;
    end.message = message;

    lsp::notifications::Progress::Params params;
    params.token = m_workspaceProgressToken;
    params.value = lsp::toJson(std::move(end));
    m_messageHandler->sendNotification<lsp::notifications::Progress>(std::move(params));

    // Cleared so a Report arriving after the End - from a scan being torn down - is dropped
    // rather than reopening a finished bar.
    m_workspaceProgressToken.clear();
}

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
        const bool mergeAll = (m_config.activePredefined == "all");
        const std::string activePath =
            (m_config.activePredefined.empty() || mergeAll)
                ? std::string()
                : angel_lsp::utils::IncludeResolver::NormalizePath(m_config.activePredefined);

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
    const auto searchDirectories = SearchDirectories();
    std::optional<PhaseTimer> phase;
    phase.emplace(m_logger.get(), "include graph");

    m_includeGraph.BuildFromFiles(
        utils::WorkspaceIncludeGraph::BuildFromFilesRequest{allScriptFiles,
                                                            *searchDirectories,
                                                            roots,
                                                            [&stopToken]() { return stopToken.stop_requested(); },
                                                            {},
                                                            std::string(ImplicitIncludeExtension())});

    if (stopToken.stop_requested())
    {
        return;
    }

    LogInfo(fmt::format("Include graph built: {} script file(s)", m_includeGraph.FileCount()));
    ReportWorkspaceProgress(fmt::format("Indexed {} script file(s)", m_includeGraph.FileCount()), 60);

    phase.emplace(m_logger.get(), "configured modules");
    BuildModuleIndex();
    PurgeUnusedClosureFiles();
    IndexConfiguredModules(parser);

    WithdrawStaleModuleDiagnostics();
    AnalyzeConfiguredModules();

    phase.emplace(m_logger.get(), "rule index construction");
    m_symbolTable.EnsureRuleIndex();
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

void Server::ReadWorkspaceFiles(const angel_lsp::utils::StopFlag& stopToken)
{
    const std::vector<std::string> workspaceRoots = WorkspaceRoots();
    std::vector<std::string> roots;
    roots.reserve(workspaceRoots.size());
    for (const auto& workspaceRoot : workspaceRoots)
    {
        roots.push_back(angel_lsp::utils::UriToPath(workspaceRoot));
    }

    angel_lsp::utils::IncludeResolver::ForgetCanonicalDirectories();
    BeginWorkspaceProgress("AngelScript: indexing workspace");
    ReportWorkspaceProgress("Building the include graph", 0);

    const PhaseTimer scanTimer(m_logger.get(), "workspace scan (total)");

    struct ReanalyseOnExit
    {
        Server* server;
        ~ReanalyseOnExit()
        {
            server->SetPredefinedReady(true);
            server->m_workspaceScanComplete.store(true);
            server->ScheduleOpenDocumentsForReanalysis();
        }
    } reanalyseOnExit{this};

    WorkspaceFilesWalkResult walkResult;
    if (!CollectWorkspaceFiles(roots, stopToken, walkResult))
    {
        EndWorkspaceProgress("Cancelled");
        return;
    }

    angel_lsp::parser::AngelScriptParser backgroundParser(m_logger.get());
    if (m_config.features.enablePredefinedLoader)
    {
        if (!LoadAndProcessPredefinedStubs(walkResult.discoveredStubPaths, stopToken, backgroundParser))
        {
            EndWorkspaceProgress("Cancelled");
            return;
        }
    }
    SetPredefinedReady(true);

    BuildIncludeGraphAndModules(walkResult.allScriptFiles, roots, stopToken, backgroundParser);
    if (stopToken.stop_requested())
    {
        EndWorkspaceProgress("Cancelled");
        return;
    }

    EndWorkspaceProgress(fmt::format("{} script file(s) indexed", m_includeGraph.FileCount()));
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

void Server::HandleNotificationsWorkspace_DidChangeWorkspaceFolders(
    lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params&& params)
{
    size_t rootCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);

        for (const auto& removed : params.event.removed)
        {
            const std::string root = angel_lsp::utils::IncludeResolver::NormalizePath(removed.uri.fsPath());
            std::erase(m_workspacesRoot, root);
        }

        for (const auto& added : params.event.added)
        {
            const std::string root = angel_lsp::utils::IncludeResolver::NormalizePath(added.uri.fsPath());
            if (std::find(m_workspacesRoot.begin(), m_workspacesRoot.end(), root) == m_workspacesRoot.end())
                m_workspacesRoot.push_back(root);
        }

        rootCount = m_workspacesRoot.size();
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

        if (const auto owner = m_predefinedManager.GetUriByPath(path))
        {
            UnloadPredefinedUri(*owner);
            SetDefinedWordsFrom(path, {});
            return true;
        }
        return false;
    }

    PurgeClosureFile(UriFromPath(path));
    PurgeClosureFile(uriStr);
    return m_includeGraph.RemoveFile(path);
}

bool Server::HandleWatchedFileChanged(const std::string& path, bool isPredefined,
                                      angel_lsp::parser::AngelScriptParser& parser)
{
    if (isPredefined)
    {
        if (m_config.features.enablePredefinedLoader)
        {
            ParserPredefined(path, parser, true);
            return true;
        }
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    m_includeGraph.UpdateFile(utils::WorkspaceIncludeGraph::UpdateFileRequest{
        path, content, *SearchDirectories(), IncludeAllowedRoots(), std::string(ImplicitIncludeExtension())});

    if (const auto indexed = m_indexedUriByPath.find(path); indexed != m_indexedUriByPath.end())
    {
        const std::string indexedUri = indexed->second;
        m_symbolTable.ClearDocumentSymbols(indexedUri);
        m_scopeIndex.ClearDocument(indexedUri);
        m_callGraph.ClearDocument(indexedUri);
        m_closureDocuments.erase(indexedUri);
        IndexClosureFile(path, parser);
    }
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
}

void Server::RefreshOpenDocumentIncludes()
{
    for (const auto& [openUri, openText] : m_documentStore.GetSnapshot())
    {
        if (const std::string openPath = CanonicalPathFromUri(openUri); !openPath.empty())
        {
            m_includeGraph.UpdateFile(utils::WorkspaceIncludeGraph::UpdateFileRequest{
                openPath, openText, *SearchDirectories(), IncludeAllowedRoots(),
                std::string(ImplicitIncludeExtension())});
        }
    }
}

void Server::HandleNotificationsWorkspace_DidChangeWatchedFiles(
    lsp::notifications::Workspace_DidChangeWatchedFiles::Params&& params)
{
    angel_lsp::parser::AngelScriptParser watchedParser(m_logger.get());
    bool graphChanged = false;
    bool stubSelectionInvalidated = false;

    for (const auto& event : params.changes)
    {
        const std::string uriStr = DocumentKey(event.uri.toString());
        if (m_documentStore.IsOpen(uriStr))
        {
            continue;
        }

        const std::string path = CanonicalPathFromUri(uriStr);
        if (path.empty())
        {
            continue;
        }

        const bool isPredefined = angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension);
        if (event.type == lsp::FileChangeType::Deleted)
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

    if (!graphChanged)
    {
        return;
    }

    RefreshOpenDocumentIncludes();
    ReanalyseOpenDocuments();
}

void Server::HandleNotificationsWindow_WorkDoneProgress_Cancel(
    lsp::notifications::Window_WorkDoneProgress_Cancel::Params&& params)
{
    // The workspace scan already polls a stop flag on every file - it has to, so a folder change
    // or a shutdown can interrupt it - so honouring a cancel is a matter of setting that flag
    // rather than of building anything. What was missing was the notification, and the
    // `cancellable` flag on the progress begin, which was false.
    //
    // Compared against the token this scan announced. A client may run several progress
    // operations at once and cancel any of them; cancelling on token alone would have stopped
    // the scan because something unrelated was dismissed.
    if (std::holds_alternative<lsp::String>(params.token) &&
        std::get<lsp::String>(params.token) == m_workspaceProgressToken)
    {
        m_workspaceStop.Request();
    }
}

void Server::HandleNotificationsSetTrace(lsp::notifications::SetTrace::Params&& params)
{
    // `$/setTrace` is how a client turns verbose logging on without restarting the server, which
    // is the difference between a user being able to send a useful log and having to reproduce
    // the problem twice.
    //
    // The protocol's three values do not line up with this server's five levels, so they are
    // mapped rather than parsed: `off` is the quietest setting that still reports real
    // failures, and both verbose steps below map onto what this server actually distinguishes.
    if (!m_logger)
        return;

    using angel_lsp::utils::LogLevel;
    switch (params.value)
    {
    case lsp::TraceValue::Off:
        m_logger->SetLevel(LogLevel::Error);
        break;
    case lsp::TraceValue::Messages:
        m_logger->SetLevel(LogLevel::Info);
        break;
    case lsp::TraceValue::Verbose:
        m_logger->SetLevel(LogLevel::Debug);
        break;

    // Not a value any client sends: the generator appends MAX_VALUE to every enum it writes,
    // and the framework parks a string it did not recognise there. Leaving the level where the
    // user last put it is the right answer to "I do not know what you asked for" - named rather
    // than reached by falling out of the switch, so the compiler stops warning that this enum
    // has an unhandled member and can warn about one that matters.
    case lsp::TraceValue::MAX_VALUE:
        break;
    }
}

void Server::HandleNotificationsWorkspace_DidCreateFiles(lsp::notifications::Workspace_DidCreateFiles::Params&& params)
{
    // The third of the file-operation notifications, and the one that was missing. A file the
    // editor has just created is on no watcher's tick yet, and an `#include` naming it has been
    // resolving to nothing - so the whole module it belongs to is missing declarations.
    //
    // The first version of this guarded on `GetFilesIncluding(path)` being non-empty, which
    // reads well and cannot work: the graph has no edge INTO a file that did not exist when the
    // edge was built. That is precisely the case this notification exists for, so the guard
    // excluded the only scenario it was meant to serve. The test caught it.
    //
    // What actually has to happen is the reverse direction: the OPEN documents' directives are
    // re-resolved, because one of them now names a file that exists.
    // And the created file's own contents have to be forgotten, which is the second half of
    // this and was missing. A name can already be in the closure cache before the file exists:
    // an editor that writes a placeholder and then the real thing, a template that scaffolds
    // and fills in, a `git checkout` racing the first analysis. The closure indexer skips
    // anything m_closureDocuments already holds, so whichever read wins decides the contents
    // for good, and no later reanalysis re-reads it.
    //
    // Measured: the test for this failed 60 times in 60 runs on Linux and 0 in 40 on Windows -
    // the two platforms losing the same race on opposite sides, which is what reached CI as an
    // intermittent. Waiting longer in the test does not help, because nothing was going to
    // re-read the file.
    //
    // The watched-file handler above has done this all along; this one had not learned it.
    std::vector<std::string> createdPaths;
    for (const auto& created : params.files)
    {
        const std::string path = CanonicalPathFromUri(DocumentKey(created.uri.toString()));
        if (!path.empty() && path.ends_with(m_config.info.fileExtension))
        {
            createdPaths.push_back(path);
        }
    }

    if (createdPaths.empty())
        return;

    {
        angel_lsp::parser::AngelScriptParser createdParser(m_logger.get());
        for (const std::string& path : createdPaths)
        {
            if (const auto indexed = m_indexedUriByPath.find(path); indexed != m_indexedUriByPath.end())
            {
                const std::string indexedUri = indexed->second;
                m_symbolTable.ClearDocumentSymbols(indexedUri);
                m_scopeIndex.ClearDocument(indexedUri);
                m_callGraph.ClearDocument(indexedUri);
                m_closureDocuments.erase(indexedUri);
                IndexClosureFile(path, createdParser);
            }
        }
    }

    const auto searchDirectories = SearchDirectories();
    for (const auto& [openUri, text] : m_documentStore.GetSnapshot())
    {
        const std::string openPath = CanonicalPathFromUri(openUri);
        if (!openPath.empty())
        {
            m_includeGraph.UpdateFile(utils::WorkspaceIncludeGraph::UpdateFileRequest{
                openPath, text, *searchDirectories, IncludeAllowedRoots(), {}});
        }
    }

    ReanalyseOpenDocuments();
}

void Server::HandleNotificationsWorkspace_DidDeleteFiles(lsp::notifications::Workspace_DidDeleteFiles::Params&& params)
{
    // The editor tells us directly rather than through the file watcher, which means it arrives
    // even when the watcher's own glob would have missed the file, and it arrives once rather
    // than as whatever burst the filesystem happened to produce.
    bool anythingChanged = false;

    for (const auto& deleted : params.files)
    {
        const std::string uriStr = DocumentKey(deleted.uri.toString());
        const std::string path = CanonicalPathFromUri(uriStr);
        if (path.empty())
            continue;

        PurgeClosureFile(uriStr);
        anythingChanged = m_includeGraph.RemoveFile(path) || anythingChanged;
        m_documentStore.RemoveClientUri(uriStr);
    }

    if (!anythingChanged)
        return;

    ReanalyseOpenDocuments();
}

void Server::HandleNotificationsWorkspace_DidRenameFiles(lsp::notifications::Workspace_DidRenameFiles::Params&& params)
{
    angel_lsp::parser::AngelScriptParser renameParser(m_logger.get());
    std::vector<lsp::TextDocumentEdit> documentEdits;
    bool anythingChanged = false;

    for (const auto& renamed : params.files)
    {
        const std::string oldUri = DocumentKey(renamed.oldUri.toString());
        const std::string newUri = DocumentKey(renamed.newUri.toString());
        const std::string oldPath = CanonicalPathFromUri(oldUri);
        const std::string newPath = CanonicalPathFromUri(newUri);
        if (oldPath.empty() || newPath.empty())
            continue;

        // Collected BEFORE the old file leaves the graph: the edge that says who included it is
        // the only record of which files need rewriting, and RemoveFile takes it with it.
        const std::vector<std::string> includers = m_includeGraph.GetFilesIncluding(oldPath);

        PurgeClosureFile(oldUri);
        m_includeGraph.RemoveFile(oldPath);
        m_documentStore.RemoveClientUri(oldUri);
        anythingChanged = true;

        for (const std::string& includer : includers)
        {
            auto edit = BuildIncludeRewrite(includer, oldPath, newPath);
            if (edit.has_value())
                documentEdits.push_back(std::move(*edit));
        }

        // The file at its new name is indexed only if something already reached it - the same
        // rule the watched-files handler applies, and for the same reason: reading every
        // renamed file off disk would turn a directory rename into a full workspace parse.
        if (!includers.empty() || m_indexedUriByPath.contains(oldPath))
            IndexClosureFile(newPath, renameParser);
    }

    if (!documentEdits.empty())
    {
        using DocumentChange = lsp::OneOf<lsp::TextDocumentEdit, lsp::CreateFile, lsp::RenameFile, lsp::DeleteFile>;
        lsp::WorkspaceEdit workspaceEdit;
        lsp::Array<DocumentChange> changes;
        for (auto& edit : documentEdits)
            changes.push_back(DocumentChange(std::move(edit)));
        workspaceEdit.documentChanges = std::move(changes);

        lsp::ApplyWorkspaceEditParams applyParams;
        applyParams.label = std::string("Update #include paths");
        applyParams.edit = std::move(workspaceEdit);

        // Sent rather than applied: the edit belongs to the editor's undo stack, and a server
        // writing the files itself would leave the user unable to undo a rename's consequences
        // along with the rename.
        m_messageHandler->sendRequest<lsp::requests::Workspace_ApplyEdit>(
            std::move(applyParams), [](auto&&) {}, [](const auto&) {});
    }

    if (anythingChanged)
        ReanalyseOpenDocuments();
}

std::optional<lsp::TextDocumentEdit> Server::BuildIncludeRewrite(const std::string& includerPath,
                                                                 const std::string& oldTargetPath,
                                                                 const std::string& newTargetPath)
{
    // Read from the editor's buffer when it has one - it may hold unsaved edits, and rewriting
    // against the copy on disk would produce an edit whose line numbers do not match what the
    // user is looking at.
    const std::string includerUri = UriFromPath(includerPath);
    std::string text;
    if (auto open = FindDocumentText(includerUri))
    {
        text = *open;
    }
    else
    {
        std::ifstream file(includerPath, std::ios::binary);
        if (!file.is_open())
            return std::nullopt;
        text.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // The path as the directive should now spell it: relative to the including file, with
    // forward slashes, which is how every #include in the corpus is written and what
    // ResolveIncludePath will read back.
    std::error_code relativeError;
    std::filesystem::path relative = std::filesystem::relative(
        std::filesystem::path(newTargetPath), std::filesystem::path(includerPath).parent_path(), relativeError);
    if (relativeError || relative.empty())
        return std::nullopt;
    const std::string replacement = relative.generic_string();

    std::vector<lsp::TextEdit> edits;
    for (const auto& directive : angel_lsp::utils::IncludeResolver::ExtractIncludes(text))
    {
        const std::string resolved = angel_lsp::utils::IncludeResolver::ResolveIncludePath(
            angel_lsp::utils::IncludeResolveRequest{directive.rawPath, includerPath, *SearchDirectories(),
                                                    IncludeAllowedRoots(), ImplicitIncludeExtension()});
        if (resolved != oldTargetPath)
            continue;

        const std::string_view line = angel_lsp::utils::GetLine(text, static_cast<uint32_t>(directive.line));
        if (auto edit = TryBuildDirectiveEdit(line, directive, replacement))
        {
            edits.push_back(std::move(*edit));
        }
    }

    if (edits.empty())
        return std::nullopt;

    lsp::TextDocumentEdit documentEdit;
    lsp::OptionalVersionedTextDocumentIdentifier identifier;
    identifier.uri = lsp::DocumentUri(lsp::Uri::parse(includerUri));
    documentEdit.textDocument = identifier;
    for (auto& edit : edits)
    {
        documentEdit.edits.push_back(std::move(edit));
    }

    return documentEdit;
}

} // namespace angel_lsp
