#pragma once

#include "i18n/i18n.h"
#include "config/ServerConfig.h"
#include "utils/LspLogger.h"
#include "utils/PositionEncoding.h"
#include "utils/PreprocessorRegions.h"
#include "utils/StopFlag.h"
#include "utils/WorkspaceIncludeGraph.h"
#include "parser/AngelScriptParser.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/CallGraph.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalyzer.h"
#include "features/formatting/FormattingHandler.h"

#include <lsp/messages.h>
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/io/stream.h>
#include <lsp/messagehandler.h>
#include <ankerl/unordered_dense.h>

#include <tree_sitter/api.h>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace angel_lsp
{
    class Server
    {
    private:
        angel_lsp::config::ServerConfig m_config;
        std::unique_ptr<lsp::Connection> m_connection;
        std::unique_ptr<lsp::MessageHandler> m_messageHandler;
        bool m_running;

        // ---- Runtime-mutable configuration ------------------------------------------------
        // Everything else in m_config is written once in the constructor and can be read from any
        // thread. These three are not: didChangeWorkspaceFolders and didChangeConfiguration both
        // rewrite them on the message loop while the workspace scan and the analysis thread are
        // reading them. Reassigning a std::vector<std::string> frees the buffer a worker may be
        // iterating, so they live behind this mutex and are reached only through the accessors
        // below - never off m_config directly.
        mutable std::mutex m_runtimeConfigMutex;
        std::vector<std::string> m_workspacesRoot;

        // Held by shared_ptr rather than by value so a reader can keep the buffer alive for the
        // duration of its call without copying every string, and a writer can swap in a new list
        // without waiting for readers to finish.
        std::shared_ptr<const std::vector<std::string>> m_searchDirectories;
        std::string m_engineProfile;

        /**
         * @brief Whether a block's opening brace goes on the statement line (K&R) or its own.
         *
         * An atomic bool rather than a member of the mutex-guarded set above: it is one word, the
         * formatting handlers read it on the message loop, and didChangeConfiguration writes it
         * there too. Nothing frees a buffer under a reader, which is what that mutex is for.
         */
        std::atomic<bool> m_formatBraceStyleKR{ false };

        std::unique_ptr<angel_lsp::i18n::I18n> m_i18n;
        std::thread m_workspaceThread;

        /** @brief Cancels the workspace scan. Rearmed only after the thread reading it was joined. */
        angel_lsp::utils::StopFlag m_workspaceStop;
        std::mutex m_messageHandlerMutex;
        std::unique_ptr<angel_lsp::utils::LspLogger> m_logger;
        std::unique_ptr<angel_lsp::parser::AngelScriptParser> m_parser;
        angel_lsp::analysis::SymbolTable m_symbolTable;
        std::unique_ptr<angel_lsp::analysis::SymbolCollector> m_symbolCollector;
        angel_lsp::analysis::ScopeIndex m_scopeIndex;

        /**
         * @brief Workspace-wide call index, maintained beside the scope trees.
         *
         * Kept out of the SymbolTable on purpose: a call is not a declaration, and folding
         * hundreds per file into the table would grow it by an order of magnitude and slow every
         * rule that walks it to serve one feature. See analysis/CallGraph.h.
         */
        angel_lsp::analysis::CallGraphIndex m_callGraph;
        std::unique_ptr<angel_lsp::analysis::LocalScopeCollector> m_localScopeCollector;
        std::unique_ptr<angel_lsp::analysis::SemanticAnalyzer> m_semanticAnalyzer;
        ankerl::unordered_dense::map<std::string, std::string> m_openDocuments;

        /**
         * @brief DocumentKey -> the URI spelling the client last used for that document.
         *
         * Diagnostics have to go back out under the client's own spelling. `PathToUri` writes
         * `file:///E:/dir/f.as` while VS Code sends `file:///e%3A/dir/f.as`, and those are
         * different strings to a client matching a notification to an open editor - so keying
         * internally by the canonical form and publishing under it would have quietly stopped
         * diagnostics from appearing at all.
         */
        ankerl::unordered_dense::map<std::string, std::string> m_clientUriByKey;
        std::unordered_map<std::string, TSTree*> m_documentTrees;
        std::mutex m_predefinedMutex;
        ankerl::unordered_dense::set<std::string> m_predefinedUris;

        // Canonical filesystem path -> the URI that predefined file is currently indexed under.
        // The workspace scan synthesises a URI from the path while the client sends its own
        // spelling on didOpen (percent-encoded drive letter, differing case). Keying the loaded
        // set by URI alone let the same stub file be collected once per spelling, which showed
        // every predefined declaration twice in hover, completion and signature help.
        // Guarded by m_predefinedMutex.
        ankerl::unordered_dense::map<std::string, std::string> m_predefinedUriByPath;


        /**
         * @brief The last semantic token payload handed to the client for a document.
         *
         * Kept so a delta request can be answered with the difference instead of the whole stream.
         * The id is what the client echoes back; when it does not match what is cached - because
         * the document was closed, or another request overwrote the entry - the full stream is
         * sent instead, which is always a valid answer to a delta request.
         */
        struct SemanticTokensSnapshot
        {
            std::string resultId;
            std::vector<lsp::uint> data;
        };

        ankerl::unordered_dense::map<std::string, SemanticTokensSnapshot> m_semanticTokensCache;
        uint64_t m_semanticTokensRevision = 0;

        /**
         * @brief The last diagnostics computed for a document, as they went out on the wire.
         *
         * Pull diagnostics (`textDocument/diagnostic`) are answered from here rather than by
         * running the analyzer on the message loop. That is not an optimisation: symbol
         * collection replaces whole-document state in m_symbolTable, and the analysis thread is
         * already doing exactly that on its own schedule. Two of them at once is a data race.
         *
         * Stored post-conversion - excluded `#if` lines already dropped, ranges already in the
         * client's encoding - so a pulled diagnostic and a pushed one cannot disagree. They come
         * from the same vector.
         */
        struct DiagnosticsSnapshot
        {
            std::string resultId;
            std::vector<lsp::Diagnostic> items;
        };

        // Written by the analysis thread through PublishDiagnostics, read by the message loop
        // answering a pull. Its own mutex rather than m_analysisMutex: that one is held across the
        // debounce wait, and a pull would block behind a sleeping worker.
        mutable std::mutex m_diagnosticsCacheMutex;
        ankerl::unordered_dense::map<std::string, DiagnosticsSnapshot> m_diagnosticsCache;
        uint64_t m_diagnosticsRevision = 0;

        // Per-rule severity overrides, typed from m_config.diagnosticSeverities at startup and
        // handed to every SemanticAnalysisRequest. Empty means "leave every rule at its own
        // severity", which is why BuildAnalysisRequest passes nullptr rather than an empty map.
        ankerl::unordered_dense::map<std::string, angel_lsp::analysis::DiagnosticSeverity> m_diagnosticSeverities;

        angel_lsp::utils::WorkspaceIncludeGraph m_includeGraph;

        // Debounced re-analysis. Reparsing is cheap and stays on the message loop so requests
        // always see a current tree, but symbol collection, scope building and semantic analysis
        // rebuild whole-document state and are far too heavy to run on every keystroke of a
        // 3000-line file. They are queued here instead and run once editing pauses.
        std::thread m_analysisThread;
        std::mutex m_analysisMutex;
        std::condition_variable m_analysisCv;
        ankerl::unordered_dense::map<std::string, std::string> m_pendingAnalysis;
        uint64_t m_analysisRevision = 0;
        bool m_analysisStop = false;


        // Files pulled in because some open document's #include module needs them, keyed by the URI
        // they were indexed under. Their text is kept so position conversion can reach them and so a
        // second open document in the same module does not re-read them off disk.
        ankerl::unordered_dense::map<std::string, std::string> m_closureDocuments;

        // Open document URI -> the closure URIs indexed on its behalf. Closure files outlive the
        // document that pulled them in whenever another open document still needs them, so they are
        // released by reference rather than on didClose.
        ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_openDocumentClosures;

        // Canonical filesystem path -> the URI that path is currently indexed under. The client's
        // spelling of a file URI and the one synthesised from a path need not match byte for byte,
        // and indexing the same file under both would duplicate every symbol in it.
        ankerl::unordered_dense::map<std::string, std::string> m_indexedUriByPath;

        // Negotiated in HandleRequestsInitialized. UTF-16 is the protocol default and the only
        // value a server may assume when the client stays silent; UTF-8 makes every conversion in
        // utils/PositionEncoding.h an identity, because it is what Tree-sitter reports natively.
        angel_lsp::utils::PositionEncoding m_positionEncoding = angel_lsp::utils::PositionEncoding::Utf16;

        /**
         * @brief Whether the client renders completion snippets, from its initialize capabilities.
         *
         * Only read by completion, and only to decide whether a template class may be offered as
         * `array<${1:T}>`. Defaults to false so a client that says nothing gets the plain name
         * rather than the placeholder syntax printed literally into its buffer.
         */
        bool m_snippetSupport = false;

        /**
         * @brief Whether the client asked for server-initiated progress, from its capabilities.
         *
         * `window.workDoneProgress`. A client that did not advertise it will not have a place to
         * put the notifications, so none are sent - the spec is explicit that a server must create
         * its own token through `window/workDoneProgress/create` first, and that request only
         * exists where the client supports it.
         */
        bool m_workDoneProgressSupport = false;

        /**
         * @brief Reports the workspace scan's progress to the client, if it can show it.
         *
         * The scan reads and indexes every script and stub under every workspace folder before any
         * cross-file symbol resolves, and until now it did all of that silently: on a large
         * workspace the server simply appears to know nothing for a while, which reads as broken
         * rather than busy.
         *
         * Each call is a no-op when the client did not advertise support, so the scan does not have
         * to care.
         */
        void BeginWorkspaceProgress(const std::string &title);
        void ReportWorkspaceProgress(const std::string &message, unsigned percentage);
        void EndWorkspaceProgress(const std::string &message);

        /** @brief Token for the scan's progress, unique per scan so a restart does not reuse one. */
        std::string m_workspaceProgressToken;
        unsigned m_workspaceProgressCounter = 0;

    public:
        /**
         * @brief Constructs the server over a JSON-RPC transport.
         * @param config Parsed configuration.
         * @param stream Transport to speak LSP over. Defaults to the process's stdio, which is what
         *        an editor launches; an injected stream is what makes this class testable at all,
         *        since taking over the test process's stdin and stdout is not an option.
         */
        Server(const angel_lsp::config::ServerConfig &config,
               lsp::io::Stream &stream = lsp::io::standardIO());
        ~Server();

        void Run();
        void InitHandles();

        /**
         * @brief Snapshot of the workspace folder URIs. Safe to call from any thread.
         *
         * Returns a copy on purpose: the caller may iterate it while the message loop adds or
         * removes a folder, and a reference into the live vector would dangle the moment it did.
         */
        std::vector<std::string> WorkspaceRoots() const;

        /**
         * @brief Current `#include` search directories. Safe to call from any thread.
         *
         * The handle keeps that revision of the list alive for as long as it is held, so a
         * concurrent didChangeConfiguration cannot pull it out from under an in-flight resolve.
         */
        std::shared_ptr<const std::vector<std::string>> SearchDirectories() const;

        /** @brief Current engine profile name. Safe to call from any thread. */
        std::string EngineProfile() const;

        /**
         * @brief Line ranges of `#if` blocks the preprocessor drops, for the configured defines.
         *
         * See utils/PreprocessorRegions.h. m_config.definedWords is written once at startup, so
         * this is safe to call from the analysis thread.
         */
        std::vector<angel_lsp::utils::ExcludedLineRange> ExcludedLineRanges(const std::string &text) const;

        /**
         * @brief Directories an `#include` in this workspace is permitted to resolve into.
         *
         * Workspace folders plus the configured search directories plus the parent directory of
         * each explicitly configured predefined stub - the three places a script may legitimately
         * include from. Everything that resolves a directive passes this to IncludeResolver, so an
         * absolute path or a `../` walk that leaves the workspace resolves to nothing instead of
         * reading, indexing and then serving back an arbitrary file off the user's disk.
         *
         * Safe to call from any thread; recomputed per call from the guarded accessors, which is
         * cheap next to the filesystem work each resolve does anyway.
         */
        std::vector<std::string> IncludeAllowedRoots() const;

        auto HandleRequestsInitialized(lsp::requests::Initialize::Params &&params);
        void HandleNotificationsInitialized(lsp::notifications::Initialized::Params &&params);
        auto HandleRequestsShutdown();
        void HandleNotificationsExit();
        void HandleNotificationsWorkspace_DidChangeConfiguration(lsp::notifications::Workspace_DidChangeConfiguration::Params &&params);

        /**
         * @brief Reacts to files created, changed or deleted outside the editor.
         *
         * Without this the index only ever learns about a file the user opened: switching branches,
         * pulling, or generating scripts from a build step would leave every stale symbol in place
         * until the file happened to be opened again.
         *
         * Documents the client has open are skipped - their in-editor buffer is authoritative and
         * may hold unsaved edits the copy on disk does not.
         */
        void HandleNotificationsWorkspace_DidChangeWatchedFiles(lsp::notifications::Workspace_DidChangeWatchedFiles::Params &&params);

        /**
         * @brief Tracks folders added to or removed from a multi-root workspace.
         *
         * Both the include graph and the predefined-stub scan are scoped to the known roots, so a
         * folder added after initialize would otherwise stay invisible for the whole session.
         */
        void HandleNotificationsWorkspace_DidChangeWorkspaceFolders(lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params &&params);

        /**
         * @brief Restarts the workspace scan on the background thread, cancelling any run in flight.
         *
         * Shared by every event that invalidates the whole graph (search paths changed, workspace
         * folders changed): which files a directive resolves to depends on both, so every edge is
         * suspect and a full rescan is the only correct answer.
         */
        void RestartWorkspaceScan();

        /**
         * @brief Computes a document's full token stream, encodes it, and records it for delta use.
         * @param uriStr Document URI.
         * @param text Document text, needed to encode byte columns into the client's encoding.
         * @return Tokens carrying the freshly minted result id.
         */
        lsp::SemanticTokens ComputeAndCacheSemanticTokens(const std::string &uriStr, const std::string &text);
        void HandleNotificationsTextDocument_DidSave(lsp::notifications::TextDocument_DidSave::Params &&params);
        void HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params &&params);
        void HandleNotificationsTextDocument_DidChange(lsp::notifications::TextDocument_DidChange::Params &&params);
        void HandleNotificationsTextDocument_DidClose(lsp::notifications::TextDocument_DidClose::Params &&params);
        /**
         * @brief Cancels any scan in flight and starts a fresh one on the workspace thread.
         *
         * The stop flag is shared by every generation of that thread, so the previous one has to be
         * joined before it is rearmed: clearing it while the old scan still runs would leave that
         * scan with a cancellation it can no longer see, and two scans reading the workspace at
         * once. Every start goes through here for that reason.
         */
        void StartWorkspaceScan();

        void ReadWorkspaceFiles(const angel_lsp::utils::StopFlag &stopToken);
        /**
         * @brief Reads a predefined stub off disk and indexes it.
         * @param forceReload Re-collect even when this server already owns the file, which is what
         *        a change reported by the file watcher needs.
         */
        void ParserPredefined(const std::string &filePath, angel_lsp::parser::AngelScriptParser &parser, bool forceReload = false);

        /**
         * @brief Loads the predefined stubs named by ServerConfig::predefinedFiles.
         *
         * Separate from the workspace scan because these are the stubs the scan cannot find: a
         * host application's declarations normally ship with the application, outside every
         * workspace folder.
         *
         * @param parser Parser to reuse across all of them.
         * @param stopToken Checked between files, so shutdown does not wait on the whole list.
         */
        void LoadConfiguredPredefinedFiles(angel_lsp::parser::AngelScriptParser &parser,
                                           const angel_lsp::utils::StopFlag &stopToken);

        /**
         * @brief Loads built-in predefined stub profiles (e.g. Standard, SvenCoop, Urho3D, OpenXRay, OOTP).
         * @param parser Parser to reuse.
         * @param stopToken Checked between profiles for early exit on cancellation.
         */
        void LoadBuiltinEngineProfiles(angel_lsp::parser::AngelScriptParser &parser,
                                       const angel_lsp::utils::StopFlag &stopToken);

        /**
         * @brief Converts the configured severity names into the analyzer's enum, once at startup.
         *
         * ServerConfig is Layer 1 and cannot name a Layer 2 type, so the override map is carried
         * as strings and typed here. An unrecognised name is logged and dropped rather than
         * guessed at.
         */
        void BuildDiagnosticSeverityOverrides();

        /**
         * @brief Logs the engine properties that were configured away from AngelScript's defaults.
         *
         * These decide which diagnostics can appear at all, so an unexpectedly quiet - or
         * unexpectedly loud - session is worth being able to explain from the log alone.
         */
        void LogNonDefaultEngineProperties() const;

        /**
         * @brief Assembles the semantic-analysis request for a document.
         *
         * Every caller wires the same configuration into it, and forgetting one field is how a rule
         * silently stops running - so the assembly lives in one place.
         *
         * @param uriStr Document URI.
         * @param text Document text. Must outlive the returned request.
         * @param tree Parsed tree for that exact text, or nullptr. Must outlive the returned
         *        request: rules that inspect expressions read through it.
         * @return Request wired with the scope tree, type configuration and feature flags.
         */
        angel_lsp::analysis::SemanticAnalysisRequest BuildAnalysisRequest(const std::string &uriStr,
                                                                          const std::string &text,
                                                                          const TSTree *tree) const;

        /**
         * @brief Rebuilds one document's symbols as a single atomic replacement.
         *
         * ClearDocumentSymbols() followed by N AddSymbol() calls is not equivalent: each takes the
         * table's write lock on its own, so between them the document exists in the index with only
         * some of its symbols - or none. A reader on another thread (the analysis thread running a
         * rule that walks the whole table for a *different* document) can land in that window and
         * emit cross-file diagnostics against a file that momentarily looks empty.
         *
         * Collecting into a staging table and swapping under one lock closes the window. This is
         * the same discipline AnalyzeDocument already used; these overloads make it the default for
         * the message-loop paths too.
         *
         * @return Diagnostics produced by symbol collection.
         */
        std::vector<angel_lsp::analysis::Diagnostic> ReplaceSymbolsFromTree(const std::string &uriStr,
                                                                            const std::string &text,
                                                                            TSTree *tree);

        /** @brief ReplaceSymbolsFromTree for a caller that has source text but no parsed tree. */
        std::vector<angel_lsp::analysis::Diagnostic> ReplaceSymbolsFromSource(const std::string &uriStr,
                                                                              const std::string &text,
                                                                              angel_lsp::parser::AngelScriptParser &parser);

        /**
         * @brief Collects the document's scopes and calls, analyses it, then publishes the scopes.
         *
         * The order is the point. `auto` inference in TypeConversionChecker writes the deduced type
         * back into the scope tree so that hover, completion and the other checkers read a concrete
         * type rather than "auto". Publishing before analysing made that write a data race - the
         * analysis thread assigning a std::string that the message loop could be reading for a
         * hover at the same moment. So the tree is built privately, handed to Analyze() as
         * SemanticAnalysisRequest::mutableScopeRoot, and only swapped into the ScopeIndex once it
         * is complete. Readers keep seeing the previous revision until then: an older consistent
         * tree, never a half-written one. Same discipline as SymbolTable::ReplaceDocumentSymbols.
         *
         * @param uriStr Document URI.
         * @param text Document text the tree was parsed from.
         * @param tree Parsed tree, or nullptr - in which case the document's scopes and calls are
         *        cleared and only the table-driven rules run.
         * @return Diagnostics produced by semantic analysis.
         */
        std::vector<angel_lsp::analysis::Diagnostic> CollectScopesAndAnalyze(const std::string &uriStr,
                                                                             const std::string &text,
                                                                             const TSTree *tree);

        /**
         * @brief Claims a predefined stub file for the given URI, releasing any earlier spelling.
         *
         * Two code paths index predefined files: the background workspace scan, which synthesises
         * a URI from the path, and didOpen, which uses whatever spelling the client sent. Both can
         * name the same file, so ownership is tracked by canonical path instead of by URI.
         *
         * @param uriStr URI the caller intends to index the file under.
         * @param forceReload Claim even when this URI already owns the file - for a caller that
         *        knows the contents changed on disk.
         * @return True if the caller should collect symbols; false if the file is already indexed
         *         under this exact URI and re-collecting would only duplicate it.
         * @pre m_predefinedMutex must be held by the caller for the whole claim-and-collect span.
         */
        bool ClaimPredefinedFile(const std::string &uriStr, bool forceReload = false);
        void PublishDiagnostics(const std::string &uriStr, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics);

        /**
         * @brief PublishDiagnostics with the document text supplied explicitly.
         *
         * The analysis thread cannot look the text up itself - m_openDocuments belongs to the
         * message loop - and the text is what the ranges are converted against.
         */
        void PublishDiagnostics(const std::string &uriStr, const std::string &text, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics);

        /**
         * @brief Analyzer diagnostics as the client receives them: filtered, encoded, converted.
         *
         * The one place that translation happens. Both the push notification and the pull request
         * answer from this, which is what keeps them from drifting into two slightly different
         * answers for the same document.
         */
        std::vector<lsp::Diagnostic> ToProtocolDiagnostics(const std::string &text, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics) const;

        /**
         * @brief Answers `textDocument/diagnostic` from the cache the analysis thread fills.
         *
         * Reports `unchanged` when the client's previousResultId still matches, which is the whole
         * point of the pull model - an unedited file costs a result id and nothing else.
         *
         * A document the analyzer has not reached yet is answered with ServerCancelled and
         * `retriggerRequest`, not with an empty report. An empty report is a positive claim that
         * the file is clean, and the server does not know that yet.
         */
        lsp::requests::TextDocument_Diagnostic::Result HandleRequestsTextDocument_Diagnostic(lsp::requests::TextDocument_Diagnostic::Params &&params);

        /**
         * @brief Answers `workspace/diagnostic` for every document the server has already analysed.
         *
         * Deliberately not a workspace scan. It reports what is known - open documents and the
         * `#include` closure files pulled in on their behalf - rather than parsing the tree from
         * scratch, which on a 1,061-file corpus is minutes of work for a request the client sends
         * on a timer.
         */
        lsp::requests::Workspace_Diagnostic::Result HandleRequestsWorkspace_Diagnostic(lsp::requests::Workspace_Diagnostic::Params &&params);

        /**
         * @brief Full text of an indexed document, or nullptr when the server holds none.
         *
         * Needed by every position conversion: translating a Tree-sitter byte column into the
         * client's encoding requires the bytes of that line. Results pointing at a document the
         * server has no text for are handed back in byte columns unchanged - wrong only on
         * non-ASCII lines, and strictly better than dropping the result.
         */
        const std::string *FindDocumentText(const std::string &uri) const;

        /**
         * @brief Canonical filesystem path behind a document URI, or empty for a non-file URI.
         *
         * The include graph is keyed by path while the rest of the server is keyed by URI, and the
         * same file can legitimately arrive spelled several ways (percent-encoded drive letters,
         * mixed separators). Everything crossing between the two goes through this.
         */
        static std::string CanonicalPathFromUri(const std::string &uriStr);

        /**
         * @brief The key every map in this server uses for a document.
         *
         * One file arrives spelled several ways and they must not become several documents. VS Code
         * sends `file:///e%3A/dir/f.as`; the workspace scan synthesises `file:///E:/dir/f.as` from
         * the path it walked; an `#include` resolves to a third spelling again. Keyed raw, an edit
         * to a file opened one way left the copy indexed the other way stale, and the predefined
         * loader had already needed a private map of its own to work around exactly this.
         *
         * Answers the raw string unchanged for anything that is not a file URI - `untitled:` has no
         * fsPath, and a document being edited before it is saved has to keep working.
         *
         * @warning This is a KEY, not something to send back. It is not the spelling the client
         *          uses, so a diagnostic published under it may not reach the document the user is
         *          looking at. PublishDiagnostics translates it back through m_clientUriByKey.
         */
        static std::string DocumentKey(const std::string &uriStr);

        void HandleNotificationsWorkspace_DidRenameFiles(lsp::notifications::Workspace_DidRenameFiles::Params &&params);

        /**
         * @brief Stops the workspace scan when the user dismisses its progress notification.
         *
         * The scan already polls a stop flag on every file so a folder change or a shutdown can
         * interrupt it; this is that flag reached from the other side. Compared against the token
         * the scan announced, because a client may run several progress operations at once.
         */
        void HandleNotificationsWindow_WorkDoneProgress_Cancel(lsp::notifications::Window_WorkDoneProgress_Cancel::Params &&params);

        /**
         * @brief Raises or lowers log verbosity without a restart.
         *
         * The protocol's three trace values are mapped onto this server's levels rather than parsed
         * into them - `off` still reports real failures, because a log that hides errors is not
         * what a user asking for less noise meant.
         */
        void HandleNotificationsSetTrace(lsp::notifications::SetTrace::Params &&params);

        /**
         * @brief Re-resolves open documents' `#include` directives when a script file appears.
         *
         * Deliberately the reverse direction from what it first looks like. The graph has no edge
         * INTO a file that did not exist when the edge was built, so asking who includes the new
         * file answers nobody - which is exactly the case this notification exists for. What
         * changed is that an OPEN document's directive now names something real.
         */
        void HandleNotificationsWorkspace_DidCreateFiles(lsp::notifications::Workspace_DidCreateFiles::Params &&params);
        void HandleNotificationsWorkspace_DidDeleteFiles(lsp::notifications::Workspace_DidDeleteFiles::Params &&params);

        /**
         * @brief One file's `#include` lines rewritten to a renamed target, or nullopt.
         *
         * Returns an edit for the editor to apply rather than writing the file: the change belongs
         * on the undo stack beside the rename that caused it, and a server editing files behind the
         * user's back is not something they can undo.
         */
        std::optional<lsp::TextDocumentEdit> BuildIncludeRewrite(const std::string &includerPath,
                                                                 const std::string &oldTargetPath,
                                                                 const std::string &newTargetPath);

        /** @brief Recomputes every open document's module closure and re-diagnoses it. */
        void ReanalyseOpenDocuments();

        /**
         * @brief Document URI for a filesystem path, in the spelling used for synthesised entries.
         */
        static std::string UriFromPath(const std::string &path);

        /** @brief The brace style the formatting handlers should use right now. */
        features::BraceStyle CurrentBraceStyle() const
        {
            return m_formatBraceStyleKR.load(std::memory_order_relaxed) ? features::BraceStyle::KAndR
                                                                       : features::BraceStyle::Allman;
        }

        /**
         * @brief Indexes every other file in the opened document's #include module.
         *
         * AngelScript modules are composed textually, so a file can use declarations from the file
         * that includes it. Opening one member of a module therefore has to index the whole module,
         * upwards as well as downwards - see WorkspaceIncludeGraph::GetModuleClosure.
         */
        void IndexModuleClosure(const std::string &openUriStr);

        /**
         * @brief Drops the closure files an open document pulled in, unless another open document
         *        still needs them.
         */
        void ReleaseModuleClosure(const std::string &openUriStr);

        /**
         * @brief Parses one closure file and adds its symbols and scopes to the index.
         */
        void IndexClosureFile(const std::string &path, angel_lsp::parser::AngelScriptParser &parser);

        /**
         * @brief Removes a closure file's symbols, scopes and cached text.
         */
        void PurgeClosureFile(const std::string &uriStr);

        /**
         * @brief Appends a warning for every #include in the document that resolves to no file.
         *
         * Lives here rather than in SemanticAnalyzer because resolution depends on the configured
         * search directories, which are server state rather than anything the AST knows about.
         */
        void AppendIncludeDiagnostics(const std::string &uriStr, const std::string &text, std::vector<angel_lsp::analysis::Diagnostic> &diagnostics) const;

        /**
         * @brief Queues a document for re-analysis once the user stops typing.
         *
         * Coalescing is by URI, so a burst of keystrokes collapses into a single run against the
         * latest text.
         */
        void ScheduleAnalysis(const std::string &uriStr, const std::string &text);

        /**
         * @brief Analysis worker: waits for a quiet period, then drains the queue.
         */
        void RunAnalysisLoop();

        /**
         * @brief Rebuilds symbols, scopes and diagnostics for one document and publishes them.
         *
         * Takes its own copy of the text and parses its own tree: the message loop owns the
         * TSTree in m_documentTrees and deletes it on the next edit, so touching it from here
         * would be a use-after-free.
         */
        void AnalyzeDocument(const std::string &uriStr, const std::string &text,
                             angel_lsp::parser::AngelScriptParser &parser);



        /**
         * @brief Converts handler output from Tree-sitter byte columns into the client's encoding.
         *
         * Feature handlers build their ranges straight from TSNode points, so everything they
         * return is in byte columns. These overloads walk each result shape and rewrite it in
         * place; all of them return immediately when UTF-8 was negotiated, which makes the whole
         * layer free on clients that speak Tree-sitter's own coordinates.
         *
         * The `text` overloads take the source of the single document a result belongs to. The
         * `AcrossDocuments` ones cover results that can point into other files (references,
         * workspace symbols, multi-file rename edits) and look each document's text up by URI.
         */
        void EncodeIn(std::string_view text, lsp::Range &range) const;
        void EncodeIn(std::string_view text, lsp::Hover &hover) const;
        void EncodeIn(std::string_view text, std::vector<lsp::TextEdit> &edits) const;
        void EncodeIn(std::string_view text, std::vector<lsp::DocumentHighlight> &highlights) const;
        void EncodeIn(std::string_view text, std::vector<lsp::FoldingRange> &ranges) const;
        void EncodeIn(std::string_view text, std::vector<lsp::DocumentLink> &links) const;
        void EncodeIn(std::string_view text, std::vector<lsp::InlayHint> &hints) const;
        void EncodeIn(std::string_view text, std::vector<lsp::DocumentSymbol> &symbols) const;
        void EncodeIn(std::string_view text, std::vector<lsp::CodeLens> &lenses) const;
        void EncodeIn(std::string_view text, lsp::PrepareRenameResult &result) const;
        void EncodeAcrossDocuments(std::vector<lsp::Location> &locations) const;
        void EncodeAcrossDocuments(std::vector<lsp::SymbolInformation> &symbols) const;
        void EncodeAcrossDocuments(lsp::WorkspaceEdit &edit) const;
        void EncodeAcrossDocuments(std::vector<lsp::CodeAction> &actions) const;

        /**
         * @brief Converts the two ranges a hierarchy item carries, against its own document.
         *
         * A hierarchy walks across files the user never opened, so the item's text is looked up by
         * URI like every other AcrossDocuments conversion. Templated because CallHierarchyItem and
         * TypeHierarchyItem share the shape and nothing else - the protocol declares them as two
         * unrelated structs.
         */
        template <typename ItemT>
        void EncodeItemRanges(ItemT &item) const
        {
            if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
                return;

            if (const std::string *text = FindDocumentText(item.uri.toString()))
            {
                EncodeIn(*text, item.range);
                EncodeIn(*text, item.selectionRange);
            }
        }

        /**
         * @brief Converts a list of ranges that all belong to one named document.
         *
         * The call hierarchy's fromRanges are ranges in the caller, which is not always the
         * document the entry points at - so the document is named explicitly rather than taken
         * from the item.
         */
        void EncodeRangesIn(const std::string &uri, std::vector<lsp::Range> &ranges) const;

    };
}