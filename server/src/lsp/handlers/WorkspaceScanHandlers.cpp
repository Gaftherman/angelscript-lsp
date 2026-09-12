#include "lsp/Server.h"
#include "utils/Utils.h"
#include "utils/WorkspaceScan.h"
#include "utils/Timer.h"
#include "utils/PreprocessorRegions.h"
#include "utils/IncludeResolver.h"
#include <spdlog/fmt/fmt.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <optional>

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
            PhaseTimer(angel_lsp::utils::LspLogger *logger, std::string phase)
                : m_logger(logger), m_phase(std::move(phase)),
                  m_start(std::chrono::steady_clock::now())
            {
            }

            ~PhaseTimer()
            {
                if (m_logger == nullptr)
                {
                    return;
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - m_start);
                m_logger->LogInfo(fmt::format("Startup phase '{}': {} ms", m_phase, elapsed.count()));
            }

            PhaseTimer(const PhaseTimer &) = delete;
            PhaseTimer &operator=(const PhaseTimer &) = delete;

        private:
            angel_lsp::utils::LspLogger *m_logger;
            std::string m_phase;
            std::chrono::steady_clock::time_point m_start;
        };
    }

    void Server::BeginWorkspaceProgress(const std::string &title)
    {
        if (!m_workDoneProgressSupport)
        {
            return;
        }

        // A fresh token per scan. RestartWorkspaceScan can begin a second scan after a folder
        // change, and reusing a token would have the client fold the two into one bar that never
        // ends.
        m_workspaceProgressToken =
            "angelscript-workspace-scan-" + std::to_string(++m_workspaceProgressCounter);

        lsp::WorkDoneProgressCreateParams createParams;
        createParams.token = m_workspaceProgressToken;
        // Fire and forget: the client either makes room for the token or it does not, and either
        // way the scan carries on. Waiting on the response here would block the thread doing the
        // work for no benefit.
        m_messageHandler->sendRequest<lsp::requests::Window_WorkDoneProgress_Create>(
            std::move(createParams), [](auto &&) {}, [](const auto &) {});

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

    void Server::ReportWorkspaceProgress(const std::string &message, unsigned percentage)
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

    void Server::EndWorkspaceProgress(const std::string &message)
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

    void Server::ReadWorkspaceFiles(const angel_lsp::utils::StopFlag &stopToken)
    {
        // Snapshotted once, up front. This runs on the workspace thread while the message loop is
        // free to add or remove a folder, and iterating the live vector while it reallocates is a
        // use-after-free - the stop-and-restart in RestartWorkspaceScan happens after the mutation,
        // not before, so it never protected this.
        const std::vector<std::string> workspaceRoots = WorkspaceRoots();
        const auto searchDirectories = SearchDirectories();

        // The #include graph is what decides which files get indexed alongside an opened document,
        // so it is built regardless of the predefined-stub loader below. Only directives are parsed
        // here, never the AST, which is what keeps a full-workspace scan affordable at startup.
        std::vector<std::string> roots;
        roots.reserve(workspaceRoots.size());
        for (const auto &workspaceRoot : workspaceRoots)
            roots.push_back(angel_lsp::utils::UriToPath(workspaceRoot));

        // A rescan is the moment a remembered directory may have moved - a folder rename, a
        // settings change and a workspace-folder change all end here. Forgetting first is what keeps
        // the startup cache's stale-entry cost bounded rather than permanent.
        angel_lsp::utils::IncludeResolver::ForgetCanonicalDirectories();

        BeginWorkspaceProgress("AngelScript: indexing workspace");
        ReportWorkspaceProgress("Building the include graph", 0);

        const PhaseTimer scanTimer(m_logger.get(), "workspace scan (total)");

        // Everything opened before this scan finishes was judged against a table that did not yet
        // hold the host API - a user opens a file, the server starts, the stub loads 400 ms later,
        // and every type it declares stays "Unknown type" until the next keystroke. Measured on a
        // real Sven Co-op project: 258 diagnostics on code that builds, every one of them this.
        //
        // On a guard rather than a line at the bottom, because this function has five early
        // returns - cancellation, a disabled loader, a stop between phases - and a call at the end
        // is reached by exactly one of them. The cancelled paths are the ones that most need it: a
        // scan that gave up still leaves every open document judged against the empty table.
        //
        // Not ReanalyseOpenDocuments: that reads m_openDocuments, which belongs to the message
        // loop, and reading it from here corrupted the heap within one run. This reads the
        // snapshot and only schedules; the analysis thread does the work.
        struct ReanalyseOnExit
        {
            Server *server;
            ~ReanalyseOnExit() { server->ScheduleOpenDocumentsForReanalysis(); }
        } reanalyseOnExit{ this };
        std::optional<PhaseTimer> phase;

        angel_lsp::parser::AngelScriptParser backgroundParser(m_logger.get());

        if (m_config.features.enablePredefinedLoader)
        {
            // Built-in predefined engine profiles (e.g. Standard, SvenCoop, Urho3D, OpenXRay, OOTP)
            ReportWorkspaceProgress("Loading engine profiles", 15);
            phase.emplace(m_logger.get(), "built-in engine profiles");
            LoadBuiltinEngineProfiles(backgroundParser, stopToken);

            if (stopToken.stop_requested())
            {
                EndWorkspaceProgress("Cancelled");
                return;
            }

            // Explicitly configured stubs first. A host application's declarations usually ship with
            // the application, not with the scripts, so the scan below - which only ever walks
            // workspace folders - would never find them. ParserPredefined de-duplicates by canonical
            // path, so a stub that also happens to live inside the workspace is not indexed twice.
            ReportWorkspaceProgress("Loading predefined stubs", 30);
            phase.emplace(m_logger.get(), "configured predefined stubs");
            const std::vector<std::string> configuredPaths = LoadConfiguredPredefinedFiles(backgroundParser, stopToken);

            phase.emplace(m_logger.get(), "workspace stub discovery");

            try
            {
                std::vector<std::string> rootPaths;
                rootPaths.reserve(workspaceRoots.size());
                for (const auto &workspaceRoot : workspaceRoots)
                    rootPaths.push_back(angel_lsp::utils::UriToPath(workspaceRoot));

                // Compared as canonical paths, never as text. The setting arrives with whatever
                // spelling the client used and the walk produces the filesystem's own - different case,
                // different separators, a percent-encoded drive letter. This project already carries
                // m_clientUriByKey because that difference bit it once.
                // "all" is a request, not a path: it asks for the old behaviour of loading every stub
                // the walk finds. Spelled out rather than left as the empty default because merging two
                // stubs that both declare `string` resolves that name twice, and a user who wants that
                // should have said so.
                const bool mergeAll = m_config.activePredefined == "all";

                const std::string activePath =
                    (m_config.activePredefined.empty() || mergeAll)
                        ? std::string()
                        : angel_lsp::utils::IncludeResolver::NormalizePath(m_config.activePredefined);

                std::vector<std::string> discovered;

                // Every stub this scan decides is still in force. Anything loaded that is not in here
                // is released at the end - see UnloadUnselectedPredefinedStubs.
                std::vector<std::string> wantedPaths = configuredPaths;

                const bool completed = angel_lsp::utils::ForEachWorkspaceFile(
                    rootPaths, m_config.exclude,
                    [&stopToken]() { return stopToken.stop_requested(); },
                    [&](const std::filesystem::directory_entry &entry) {
                        if (!angel_lsp::utils::IsPredefinedFile(entry.path().string(), m_config.info.predefinedFileExtension))
                            return;

                        const std::string path = angel_lsp::utils::IncludeResolver::NormalizeWalkedPath(entry.path());
                        discovered.push_back(path);

                        if (!activePath.empty())
                        {
                            if (PathsAreSameFile(path, activePath))
                            {
                                ParserPredefined(entry.path().string(), backgroundParser);
                                wantedPaths.push_back(path);
                            }
                            return;
                        }

                        if (mergeAll)
                        {
                            ParserPredefined(entry.path().string(), backgroundParser);
                            wantedPaths.push_back(path);
                        }

                        // Neither chosen nor merging: nothing is loaded here, because which stub wins
                        // cannot be decided until the walk has seen all of them.
                    });

                // The only caller with something to close out on a cancel, which is why the walker
                // reports whether it finished rather than swallowing the distinction.
                if (!completed)
                {
                    EndWorkspaceProgress("Cancelled");
                    return;
                }

                // Sorted so the pick below is the same on every machine and every run. Directory
                // iteration order is not specified, and a stub that wins on one developer's disk and
                // loses on another's is the worst possible version of this feature.
                std::sort(discovered.begin(), discovered.end());

                std::string autoSelected;
                if (activePath.empty() && !mergeAll && !discovered.empty())
                {
                    autoSelected = discovered.front();
                    ParserPredefined(autoSelected, backgroundParser);
                    wantedPaths.push_back(autoSelected);
                }

                // Anything still loaded from a stub file this scan did not want has to go, for exactly
                // the reason the built-in profiles above do: didChangeConfiguration sets shouldRescan,
                // the rescan reaches here, and ClaimPredefinedFile refuses the URIs it has already seen
                // while nothing ever releases the one the user just switched away from. Measured before
                // this call existed - selecting a second stub left the first one's classes resolving in
                // hover and completion, unmarked, and the `#define`s it wrote still keeping `#if`
                // blocks live. Only the built-in profile half of this had ever been fixed.
                UnloadUnselectedPredefinedStubs(wantedPaths);

                {
                    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
                    m_discoveredPredefined = discovered;
                    m_effectivePredefined = autoSelected.empty() ? activePath : autoSelected;
                }

                ReportPredefinedSelection(discovered, activePath, autoSelected, mergeAll);
            }
            catch (const std::exception &e)
            {
                LogError(fmt::format("Error reading workspace files: {}", e.what()));
            }

            if (stopToken.stop_requested())
            {
                EndWorkspaceProgress("Cancelled");
                return;
            }
        }

        phase.emplace(m_logger.get(), "include graph");

        m_includeGraph.Build(roots,
                             *searchDirectories,
                             m_config.info.fileExtension,
                             [&stopToken]() { return stopToken.stop_requested(); },
                             {},
                             m_config.exclude,
                             ImplicitIncludeExtension());

        if (stopToken.stop_requested())
        {
            EndWorkspaceProgress("Cancelled");
            return;
        }

        LogInfo(fmt::format("Include graph built: {} script file(s)", m_includeGraph.FileCount()));

        // Which files a module contains is a question about the graph, so it is answered here and
        // nowhere else - every path that rebuilds the graph passes through this line.
        ReportWorkspaceProgress(
            fmt::format("Indexed {} script file(s)", m_includeGraph.FileCount()), 60);

        // Which files each configured module contains, and then their contents. A module nobody
        // has opened still has to be in the symbol table, or the module that externs its shared
        // entities cannot see them - two modules are by definition not connected by an #include,
        // so the open document's own closure never reaches across. See IndexConfiguredModules.
        phase.emplace(m_logger.get(), "configured modules");
        BuildModuleIndex();
        PurgeUnusedClosureFiles();
        IndexConfiguredModules(backgroundParser);

        // Anything published for a module that no longer claims it has to be taken back before the
        // new answers go out, or a file that changed module keeps both verdicts.
        WithdrawStaleModuleDiagnostics();
        AnalyzeConfiguredModules();

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

    void Server::HandleNotificationsWorkspace_DidChangeWorkspaceFolders(lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params &&params)
    {
        size_t rootCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);

            for (const auto &removed : params.event.removed)
            {
                const std::string root = angel_lsp::utils::IncludeResolver::NormalizePath(removed.uri.fsPath());
                std::erase(m_workspacesRoot, root);
            }

            for (const auto &added : params.event.added)
            {
                const std::string root = angel_lsp::utils::IncludeResolver::NormalizePath(added.uri.fsPath());
                if (std::find(m_workspacesRoot.begin(), m_workspacesRoot.end(), root) == m_workspacesRoot.end())
                    m_workspacesRoot.push_back(root);
            }

            rootCount = m_workspacesRoot.size();
        }

        LogInfo(fmt::format("Workspace folders changed (+{} -{}); now {} root(s), rescanning",
                                      params.event.added.size(), params.event.removed.size(), rootCount));

        // A rescan rather than an incremental patch: the include graph is rebuilt wholesale by
        // Build(), and a removed root's files have to leave the graph as much as an added root's
        // have to enter it.
        RestartWorkspaceScan();
    }

    void Server::HandleNotificationsWorkspace_DidChangeWatchedFiles(lsp::notifications::Workspace_DidChangeWatchedFiles::Params &&params)
    {
        angel_lsp::parser::AngelScriptParser watchedParser(m_logger.get());
        bool graphChanged = false;

        // Set when the stub that was in force is the one that just disappeared. See the delete
        // branch below: with nothing configured, which stub is loaded is a choice the scan made,
        // and a deleted file unmakes it.
        bool stubSelectionInvalidated = false;

        for (const auto &event : params.changes)
        {
            const std::string uriStr = DocumentKey(event.uri.toString());

            // The editor's buffer wins over the copy on disk: it may hold unsaved edits, and
            // didChange/didSave already keep it indexed.
            if (m_openDocuments.contains(uriStr))
                continue;

            const std::string path = CanonicalPathFromUri(uriStr);
            if (path.empty())
                continue;

            const bool isPredefined = angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension);


            if (event.type == lsp::FileChangeType::Deleted)
            {
                if (isPredefined)
                {
                    // Whether the stub that just vanished was the one in force. With nothing
                    // configured the scan picks the first stub it finds, and that choice has just
                    // been invalidated - so the scan has to run again and pick another. Without
                    // this a workspace with two stubs, one of them deleted, ends up with no host
                    // types at all while the other one is still sitting there on disk.
                    {
                        // Both sides normalised the same way before comparing. The event's path
                        // comes back from CanonicalPathFromUri and the selection from
                        // IncludeResolver::NormalizePath, and on Windows those disagree about the
                        // separator - so a plain comparison of the two never matched and this flag
                        // was never set.
                        const std::string deletedPath = angel_lsp::utils::IncludeResolver::NormalizePath(path);

                        std::lock_guard<std::mutex> configLock(m_runtimeConfigMutex);
                        if (!m_effectivePredefined.empty() && PathsAreSameFile(deletedPath, m_effectivePredefined))
                            stubSelectionInvalidated = true;
                    }

                    std::lock_guard<std::mutex> lock(m_predefinedMutex);
                    if (const auto owner = m_predefinedUriByPath.find(path); owner != m_predefinedUriByPath.end())
                    {
                        UnloadPredefinedUri(owner->second);

                        // A deleted stub takes its `#define`s with it, so every `#if` that was live
                        // because of one goes back to being excluded. That is a change to what the
                        // compiler would see in every open document, not just to this file, so it
                        // joins the fan-out at the end of this function - which a deleted stub did
                        // not do at all before: the branch above `continue`d without ever setting
                        // graphChanged, so removing a stub left its symbols gone and every open
                        // document still diagnosed against them until the next keystroke.
                        SetDefinedWordsFrom(path, {});
                        graphChanged = true;
                    }
                }
                else
                {
                    PurgeClosureFile(UriFromPath(path));
                    PurgeClosureFile(uriStr);
                    graphChanged = m_includeGraph.RemoveFile(path) || graphChanged;
                }
                continue;
            }

            if (isPredefined)
            {
                if (m_config.features.enablePredefinedLoader)
                {
                    ParserPredefined(path, watchedParser, /*forceReload=*/true);

                    // The stub was re-read, and every open document was judged against the old
                    // one. Editing a stub is how a user teaches this server about the types their
                    // host registers, so the diagnostics it changes are exactly the ones they are
                    // watching - and until this line they did not move until the next keystroke in
                    // some other file. The `continue` below skips the `graphChanged = true` that
                    // the ordinary path sets, so the fan-out at the end of this function never ran.
                    //
                    // Reusing that flag rather than fanning out here: a workspace can hold several
                    // stubs, and one save should reanalyse each open document once, not once per
                    // stub.
                    graphChanged = true;
                }
                continue;
            }

            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
                continue;

            const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            m_includeGraph.UpdateFile(path, content, *SearchDirectories(), IncludeAllowedRoots(),
                                      ImplicitIncludeExtension());
            graphChanged = true;

            // Only files already pulled in as part of an open document's module are re-indexed
            // here. Anything else has no symbols in the table to go stale, and reading every
            // created file off disk would turn a `git checkout` into a full workspace parse.
            if (const auto indexed = m_indexedUriByPath.find(path); indexed != m_indexedUriByPath.end())
            {
                const std::string indexedUri = indexed->second;
                m_symbolTable.ClearDocumentSymbols(indexedUri);
                m_scopeIndex.ClearDocument(indexedUri);
                m_callGraph.ClearDocument(indexedUri);
                m_closureDocuments.erase(indexedUri);
                IndexClosureFile(path, watchedParser);
            }
        }

        if (stubSelectionInvalidated)
        {
            // Which stub is loaded, with nothing configured, is a choice the workspace scan made -
            // the first it found in path order - and deleting that file unmakes it. Nothing re-made
            // it, so a workspace with two stubs, one of them deleted, was left with no host types
            // at all while the other one was still sitting there.
            //
            // Chosen here rather than by restarting the scan: a rescan would load it on the
            // workspace thread and leave every open document judged against the old table until the
            // next keystroke, which is the staleness this project has already been bitten by. One
            // file parsed on the message loop costs less and finishes before the refresh below.
            std::string replacement;
            {
                std::lock_guard<std::mutex> configLock(m_runtimeConfigMutex);

                std::erase_if(m_discoveredPredefined, [this](const std::string &candidate) {
                    return PathsAreSameFile(candidate, m_effectivePredefined);
                });

                if (!m_discoveredPredefined.empty())
                    replacement = m_discoveredPredefined.front();

                m_effectivePredefined = replacement;
            }

            if (!replacement.empty())
            {
                LogInfo(fmt::format(
                    "The predefined stub in force was deleted; using {} instead",
                    std::filesystem::path(replacement).filename().string()));

                // Not under m_predefinedMutex: ParserPredefined takes it itself, as every other
                // caller in this handler relies on.
                ParserPredefined(replacement, watchedParser);
            }
            else
            {
                LogInfo("The predefined stub in force was deleted, and no other was found");
            }

            graphChanged = true;
        }

        if (!graphChanged)
            return;

        // An #include that pointed at nothing may point at something now, and the edge that says
        // so belongs to the *including* file - which nothing here has touched. Creating helper.as
        // updated helper.as's own entry in the graph and left `#include "helper.as"` in main.as
        // exactly as unresolved as it was, so the type it declares stayed unknown until the user
        // typed something in main.as.
        //
        // Only the open documents are refreshed: they are the ones whose diagnostics are on screen,
        // and re-resolving every file in the workspace on every watched event would turn a `git
        // checkout` into a full rescan. Directives only, so it costs a scan of the text rather than
        // a parse.
        for (const auto &[openUri, openText] : m_openDocuments)
        {
            if (const std::string openPath = CanonicalPathFromUri(openUri); !openPath.empty())
                m_includeGraph.UpdateFile(openPath, openText, *SearchDirectories(), IncludeAllowedRoots(),
                                          ImplicitIncludeExtension());
        }

        // An edited #include line can move a file between modules, so every open document's
        // closure is recomputed and re-diagnosed against whatever it now sees.
        ReanalyseOpenDocuments();
    }


    void Server::HandleNotificationsWindow_WorkDoneProgress_Cancel(lsp::notifications::Window_WorkDoneProgress_Cancel::Params &&params)
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

    void Server::HandleNotificationsSetTrace(lsp::notifications::SetTrace::Params &&params)
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

    void Server::HandleNotificationsWorkspace_DidCreateFiles(lsp::notifications::Workspace_DidCreateFiles::Params &&params)
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
        for (const auto &created : params.files)
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
            for (const std::string &path : createdPaths)
            {
                if (const auto indexed = m_indexedUriByPath.find(path);
                    indexed != m_indexedUriByPath.end())
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
        for (const auto &[openUri, text] : m_openDocuments)
        {
            const std::string openPath = CanonicalPathFromUri(openUri);
            if (!openPath.empty())
                m_includeGraph.UpdateFile(openPath, text, *searchDirectories, IncludeAllowedRoots());
        }

        ReanalyseOpenDocuments();
    }

    void Server::HandleNotificationsWorkspace_DidDeleteFiles(lsp::notifications::Workspace_DidDeleteFiles::Params &&params)
    {
        // The editor tells us directly rather than through the file watcher, which means it arrives
        // even when the watcher's own glob would have missed the file, and it arrives once rather
        // than as whatever burst the filesystem happened to produce.
        bool anythingChanged = false;

        for (const auto &deleted : params.files)
        {
            const std::string uriStr = DocumentKey(deleted.uri.toString());
            const std::string path = CanonicalPathFromUri(uriStr);
            if (path.empty())
                continue;

            PurgeClosureFile(uriStr);
            anythingChanged = m_includeGraph.RemoveFile(path) || anythingChanged;
            m_clientUriByKey.erase(uriStr);
        }

        if (!anythingChanged)
            return;

        ReanalyseOpenDocuments();
    }

    void Server::HandleNotificationsWorkspace_DidRenameFiles(lsp::notifications::Workspace_DidRenameFiles::Params &&params)
    {
        angel_lsp::parser::AngelScriptParser renameParser(m_logger.get());
        std::vector<lsp::TextDocumentEdit> documentEdits;
        bool anythingChanged = false;

        for (const auto &renamed : params.files)
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
            m_clientUriByKey.erase(oldUri);
            anythingChanged = true;

            for (const std::string &includer : includers)
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
            using DocumentChange = lsp::OneOf<lsp::TextDocumentEdit, lsp::CreateFile,
                                              lsp::RenameFile, lsp::DeleteFile>;
            lsp::WorkspaceEdit workspaceEdit;
            lsp::Array<DocumentChange> changes;
            for (auto &edit : documentEdits)
                changes.push_back(DocumentChange(std::move(edit)));
            workspaceEdit.documentChanges = std::move(changes);

            lsp::ApplyWorkspaceEditParams applyParams;
            applyParams.label = std::string("Update #include paths");
            applyParams.edit = std::move(workspaceEdit);

            // Sent rather than applied: the edit belongs to the editor's undo stack, and a server
            // writing the files itself would leave the user unable to undo a rename's consequences
            // along with the rename.
            m_messageHandler->sendRequest<lsp::requests::Workspace_ApplyEdit>(
                std::move(applyParams), [](auto &&) {}, [](const auto &) {});
        }

        if (anythingChanged)
            ReanalyseOpenDocuments();
    }

    std::optional<lsp::TextDocumentEdit> Server::BuildIncludeRewrite(const std::string &includerPath,
                                                                     const std::string &oldTargetPath,
                                                                     const std::string &newTargetPath)
    {
        // Read from the editor's buffer when it has one - it may hold unsaved edits, and rewriting
        // against the copy on disk would produce an edit whose line numbers do not match what the
        // user is looking at.
        const std::string includerUri = UriFromPath(includerPath);
        std::string text;
        if (const std::string *open = FindDocumentText(includerUri))
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
        for (const auto &directive : angel_lsp::utils::IncludeResolver::ExtractIncludes(text))
        {
            const std::string resolved = angel_lsp::utils::IncludeResolver::ResolveIncludePath(
                directive.rawPath, includerPath, *SearchDirectories(), IncludeAllowedRoots(),
                ImplicitIncludeExtension());
            if (resolved != oldTargetPath)
                continue;

            const std::string_view line = angel_lsp::utils::GetLine(text, static_cast<uint32_t>(directive.line));
            const char open = directive.isAngled ? '<' : '"';
            const char close = directive.isAngled ? '>' : '"';
            const size_t openPos = line.find(open);
            if (openPos == std::string_view::npos)
                continue;
            const size_t closePos = line.find(close, openPos + 1);
            if (closePos == std::string_view::npos)
                continue;

            lsp::TextEdit edit;
            edit.range.start.line = static_cast<uint32_t>(directive.line);
            edit.range.start.character = static_cast<uint32_t>(openPos + 1);
            edit.range.end.line = static_cast<uint32_t>(directive.line);
            edit.range.end.character = static_cast<uint32_t>(closePos);
            edit.newText = replacement;
            edits.push_back(std::move(edit));
        }

        if (edits.empty())
            return std::nullopt;

        lsp::TextDocumentEdit documentEdit;
        lsp::OptionalVersionedTextDocumentIdentifier identifier;
        identifier.uri = lsp::DocumentUri(lsp::Uri::parse(includerUri));
        documentEdit.textDocument = identifier;
        for (auto &edit : edits)
            documentEdit.edits.push_back(std::move(edit));

        return documentEdit;
    }

}
