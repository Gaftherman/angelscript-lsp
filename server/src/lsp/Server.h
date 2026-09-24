#pragma once

#include "analysis/CallGraph.h"
#include "analysis/EngineProfiles.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "config/ServerConfig.h"
#include "document/Document.h"
#include "features/formatting/FormattingHandler.h"
#include "i18n/i18n.h"
#include "lsp/AnalysisScheduler.h"
#include "lsp/DocumentStore.h"
#include "lsp/ModuleIndex.h"
#include "lsp/PredefinedStubManager.h"
#include "parser/AngelScriptParser.h"
#include "utils/LspLogger.h"
#include "utils/PositionEncoding.h"
#include "utils/PreprocessorRegions.h"
#include "utils/StopFlag.h"
#include "utils/Timer.h"
#include "utils/WorkspaceIncludeGraph.h"

#include <ankerl/unordered_dense.h>
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/io/stream.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <vector>

namespace angel_lsp
{
class Server
{
  private:
    angel_lsp::config::ServerConfig m_config;
    std::unique_ptr<lsp::Connection> m_connection;
    std::unique_ptr<lsp::MessageHandler> m_messageHandler;
    std::atomic<bool> m_running{false};

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

    // The words `#if` treats as defined, from all three sources at once: the --define flag, the
    // angelscript.define setting, and `#define` in every loaded predefined stub. The stubs are
    // what make this mutable - they reload while the analysis thread is reading, so it gets the
    // same shared_ptr snapshot treatment as m_searchDirectories rather than being read off
    // m_config, which is what the old comment on ExcludedLineRanges assumed it could do.
    std::shared_ptr<const ankerl::unordered_dense::set<std::string>> m_definedWords;

    // Contributions keyed by the stub that made them, so reloading one stub replaces its own
    // words without disturbing another's. The empty key holds the flag and setting words.
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_definedWordsBySource;

    /**
     * @brief Whether a block's opening brace goes on the statement line (K&R) or its own.
     *
     * An atomic bool rather than a member of the mutex-guarded set above: it is one word, the
     * formatting handlers read it on the message loop, and didChangeConfiguration writes it
     * there too. Nothing frees a buffer under a reader, which is what that mutex is for.
     */
    std::atomic<bool> m_formatBraceStyleKR{false};
    std::atomic<uint64_t> m_configRevision{0};

    std::unique_ptr<angel_lsp::i18n::I18n> m_i18n;
    std::thread m_workspaceThread;

    /** @brief Cancels the workspace scan. Rearmed only after the thread reading it was joined. */
    angel_lsp::utils::StopFlag m_workspaceStop;

    /** @brief Set once the first workspace scan finishes; used to defer initial diagnostics. */
    std::atomic<bool> m_workspaceScanComplete{false};

    /** @brief Barrier signaling that predefined stubs and engine profiles have loaded. */
    std::atomic<bool> m_predefinedReady{false};
    mutable std::mutex m_predefinedMutex;
    mutable std::condition_variable m_predefinedCv;
    std::mutex m_messageHandlerMutex;
    std::unique_ptr<angel_lsp::utils::LspLogger> m_logger;
    std::unique_ptr<angel_lsp::parser::AngelScriptParser> m_parser;
    std::unique_ptr<angel_lsp::parser::AngelScriptParser> m_workerParser;
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
    angel_lsp::DocumentStore m_documentStore;
    angel_lsp::PredefinedStubManager m_predefinedManager;

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
        bool hasError = false;
        int version = -1;
    };

    ankerl::unordered_dense::map<std::string, SemanticTokensSnapshot> m_semanticTokensCache;
    mutable std::mutex m_semanticTokensMutex;
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

        /**
         * @brief Hash of the document text these diagnostics were computed from.
         *
         * Without it the pull handler could tell "I have an answer for this document" but not
         * "I have a *current* one", and it served whatever was last computed however old. The
         * effect was visible and confusing: finishing a statement with `;` left the missing-`;`
         * error on screen, because the pull answer still described the text from before the
         * keystroke while the push notification - a separate diagnostic collection in the
         * editor - already showed the file was fine. Typing anything else appeared to "fix" it,
         * which is the shape of a staleness bug rather than an analysis one.
         *
         * A hash rather than the text: one per open document, and the worst a collision can do
         * is serve one stale report, which is the behaviour being fixed rather than a new
         * failure.
         */
        size_t textHash = 0;
        int version = -1;
        uint64_t generation = 0;
        uint64_t configRevision = 0;
    };

    // Serializes document lifecycle mutations (didOpen, didChange, didSave, didClose)
    // with background analysis commits (CommitAnalysisResults), eliminating race windows.
    mutable std::mutex m_lifecycleMutex;

    // Optional test hook invoked right before committing analysis results, for deterministic barrier testing.
    std::function<void(const std::string& uri, int version, uint64_t generation)> m_onBeforeCommitHook;

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
    angel_lsp::ModuleIndex m_moduleIndex;

    // Debounced re-analysis. Reparsing is cheap and stays on the message loop so requests
    // always see a current tree, but symbol collection, scope building and semantic analysis
    // rebuild whole-document state and are far too heavy to run on every keystroke of a
    // 3000-line file. They are queued here instead and run once editing pauses.
    std::unique_ptr<angel_lsp::AnalysisScheduler> m_analysisScheduler;

    void SetDocumentVersion(const std::string& uriStr, int version)
    {
        m_documentStore.SetVersion(uriStr, version);
    }

    int GetDocumentVersion(const std::string& uriStr) const
    {
        return m_documentStore.GetVersion(uriStr);
    }

    void RemoveDocumentVersion(const std::string& uriStr)
    {
        m_documentStore.SetVersion(uriStr, -1);
    }

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
    void BeginWorkspaceProgress(const std::string& title);
    void ReportWorkspaceProgress(const std::string& message, unsigned percentage);
    void EndWorkspaceProgress(const std::string& message);

    /** @brief Token for the scan's progress, unique per scan so a restart does not reuse one. */
    std::string m_workspaceProgressToken;
    unsigned m_workspaceProgressCounter = 0;

    /**
     * @brief Returns the document text to be parsed and analyzed.
     */
    std::string AnalysisTextFor(const std::string& uriStr, const std::string& text) const;

  public:
    /**
     * @brief Constructs the server over a JSON-RPC transport.
     * @param config Parsed configuration.
     * @param stream Transport to speak LSP over. Defaults to the process's stdio, which is what
     *        an editor launches; an injected stream is what makes this class testable at all,
     *        since taking over the test process's stdin and stdout is not an option.
     */
    Server(const angel_lsp::config::ServerConfig& config, lsp::io::Stream& stream = lsp::io::standardIO());
    ~Server();

    void Run();

    /**
     * @brief Processes a single iteration of incoming transport messages.
     * @return True if message processed without error, false on failure or transport close.
     */
    bool ProcessIncomingMessageStep();
    void InitHandles();
    void RegisterWorkspaceHandlers();
    void RegisterTextDocumentHandlers();
    void RegisterHierarchyHandlers();
    void RegisterTokensAndFormattingHandlers();

    using OnBeforeCommitHook = std::function<void(const std::string& uri, int version, uint64_t generation)>;

    /**
     * @brief Test hook invoked immediately before acquiring lifecycle mutex to commit analysis results.
     */
    void SetOnBeforeCommitHook(OnBeforeCommitHook hook)
    {
        m_onBeforeCommitHook = std::move(hook);
    }

    /**
     * @brief Const reference to the server symbol table (used for test assertions).
     */
    const angel_lsp::analysis::SymbolTable& GetSymbolTable() const
    {
        return m_symbolTable;
    }

    /**
     * @brief Reference to the server document store (used for test assertions).
     */
    const angel_lsp::DocumentStore& GetDocumentStore() const
    {
        return m_documentStore;
    }

    /**
     * @brief Blocks the calling thread until all pending and currently executing
     *        background analysis tasks in the scheduler have finished.
     */
    void DrainQueue()
    {
        if (m_analysisScheduler)
        {
            m_analysisScheduler->DrainQueue();
        }
    }

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
     * See utils/PreprocessorRegions.h. Safe to call from the analysis thread: it reads the
     * defined words through DefinedWords(), which hands back an immutable snapshot.
     */
    std::vector<angel_lsp::utils::ExcludedLineRange> ExcludedLineRanges(const std::string& text) const;

    /** @brief Snapshot of the words `#if` treats as defined. Safe to call from any thread. */
    std::shared_ptr<const ankerl::unordered_dense::set<std::string>> DefinedWords() const;

    /**
     * @brief Records one source's `#define` contributions and rebuilds the snapshot.
     *
     * @param source Key identifying the contributor - a stub's path, or "" for the flag and
     *        the client setting. Passing an empty @p words removes that source's contribution.
     * @return True when the resulting set differs from the previous one, which is the signal
     *         that every open document's excluded ranges have changed and it needs reanalysis.
     */
    bool SetDefinedWordsFrom(const std::string& source, std::vector<std::string> words);

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
    /**
     * @brief One configured module, resolved against the include graph.
     *
     * Rebuilt whenever the graph is - which is whenever an `#include` line could have moved a
     * file between modules.
     */
    struct ModuleView
    {
        std::string name;

        /** @brief Normalised absolute path of the entry script, or empty when there is none. */
        std::string entryPath;

        /** @brief Normalised absolute path of the module's folder, or empty when there is none. */
        std::string folderPath;

        /**
         * @brief Every file in the module: the entry's include closure, the folder's scripts,
         *        or both.
         */
        ankerl::unordered_dense::set<std::string> memberPaths;

        /** @brief The subset reached from the entry point, which a folder does not claim. */
        ankerl::unordered_dense::set<std::string> closurePaths;
    };

    /**
     * @brief Which module owns a file, and which others also claimed it.
     *
     * A file belongs to exactly one module. AngelScript really does allow one file to be
     * compiled into several, so this is a simplification - and the ones that lost are reported
     * back so the server can say so rather than decide in silence.
     */
    struct ModuleClaim
    {
        const ModuleView* owner = nullptr;
        std::vector<std::string> alsoClaimedBy;
    };

    /**
     * @brief Resolves module membership for one path, most specific claim winning.
     *
     *     an entry point's include closure  >  the deepest folder  >  any folder above it
     *
     * One function, because everything else reads it: a second, subtly different copy of this
     * rule is how the answer starts depending on which caller asked.
     */
    [[nodiscard]] ModuleClaim ClaimFor(const std::string& normalizedPath) const;

    /**
     * @brief True when a normalised path sits under a normalised directory.
     *
     * Compared the way every other path comparison here is - case-insensitively on Windows,
     * where `scripts/Maps` and `scripts/maps` are one directory and comparing bytes would make
     * them two - and on a component boundary, so `scripts/map` does not contain
     * `scripts/maps/x.as`.
     */
    [[nodiscard]] static bool PathIsInside(const std::string& normalizedPath, const std::string& normalizedDirectory);

    /**
     * @brief Re-analyses and republishes every file of every configured module.
     *
     * The point of naming a module: an error in a file the entry point includes reaches the
     * Problems panel instead of waiting until that file is opened. Run on the workspace scan and
     * on save - not on every keystroke, which would re-analyse a few hundred files after each
     * typing pause and has not been measured at that size.
     *
     * Open documents are skipped. They have their own analysis, from the buffer rather than
     * from disk, and two publishers for one URI is a race whose loser publishes an older answer.
     */
    /**
     * @brief Resolves a configured module path, taking a relative one as workspace-relative.
     *
     * Absolute is taken at face value. Relative is tried against each workspace root and the
     * first that exists wins, matching how a relative predefined file is treated. Falls back to
     * the path as written when none match, so the caller's error names what the user typed.
     */
    std::string ResolveConfiguredPath(const std::string& configured) const;

    void AnalyzeConfiguredModules();

    /** @brief True when the snapshot holds this document. Safe from any thread. */
    [[nodiscard]] bool IsOpenElsewhere(const std::string& uriStr) const;

    /**
     * @brief Re-analyses every open document, from the workspace thread.
     *
     * Only schedules: the analysis thread does the work and publishes. Deliberately does NOT
     * index module closures the way ReanalyseOpenDocuments does - that touches message-loop
     * state, and the scan calling this has just indexed everything anyway.
     */
    void ScheduleOpenDocumentsForReanalysis();

    /**
     * @brief Publishes an empty list for every file that was in a module and no longer is.
     *
     * Without this a renamed module, a deleted file, or an `#include` edit that shrinks a
     * closure leaves its diagnostics in the Problems panel for the rest of the session, on files
     * the user may not be able to open to clear them by hand.
     */
    void WithdrawStaleModuleDiagnostics();

    /** @brief URIs this server has published module diagnostics for, so they can be withdrawn. */
    mutable std::mutex m_publishedForModulesMutex;
    ankerl::unordered_dense::set<std::string> m_publishedForModules;

    /** @brief The configured modules, resolved. Empty when angelscript.modules is not set. */
    std::vector<ModuleView> m_modules;

    /**
     * @brief Recomputes which files belong to which configured module.
     *
     * Cheap and rare: one GetModuleClosure per configured module, run only where the include
     * graph itself is rebuilt.
     */
    void BuildModuleIndex();

    /**
     * @brief Indexes every file of every configured module.
     *
     * Without this a module is a name over an empty set. The symbol table only ever held the
     * open document's own include closure, and two modules are by definition NOT connected by
     * an `#include` - so the entities one module shares were invisible to the other, and the
     * external-shared rule had nothing to find. It reported correct code as broken, which is
     * how the feature failed its own first test.
     *
     * Paid only when angelscript.modules is configured: describing the modules is what asks
     * for them all to be read.
     */
    void IndexConfiguredModules(angel_lsp::parser::AngelScriptParser& parser);

    /**
     * @brief Synchronizes exported module symbols into m_moduleIndex for prefix completion.
     */
    void SyncModuleIndexSymbols();

    /**
     * @brief Purges symbols and state for indexed closure files that no longer belong to
     *        any configured module and are not required by any open document's include closure.
     */
    void PurgeUnusedClosureFiles();

    /**
     * @brief Sends a warning message to the client when a module definition has an issue.
     * @param[in] text Warning message to notify and log.
     */
    void NotifyModuleWarning(const std::string& text);

    /**
     * @brief Populates directory members for a module from workspace files.
     * @param[in] folder Folder path configured for the module.
     * @param[in,out] view Target module view receiving members.
     * @param[in] workspaceFiles List of all files discovered in the workspace.
     * @return True if folder exists and members were populated, false if folder does not exist.
     */
    bool PopulateModuleFolderMembers(const std::string& folder, ModuleView& view,
                                     const std::vector<std::string>& workspaceFiles);

    /**
     * @brief Populates entry script closure for a module from the include graph.
     * @param[in] entry Entry script path configured for the module.
     * @param[in,out] view Target module view receiving entry closure paths.
     * @return True if entry file exists and closure was resolved, false if entry does not exist.
     */
    bool PopulateModuleEntryClosure(const std::string& entry, ModuleView& view);

    /**
     * @brief Resolves a single module definition against discovered workspace files.
     * @param[in] definition User-configured module definition.
     * @param[in] resolved Already resolved modules to check for duplicate names.
     * @param[in] workspaceFiles List of all files discovered in the workspace.
     * @return Fully populated module view, or std::nullopt if definition was invalid or duplicate.
     */
    [[nodiscard]] std::optional<ModuleView>
    ResolveModuleDefinition(const config::ServerConfig::ModuleDefinition& definition,
                            const std::vector<ModuleView>& resolved, const std::vector<std::string>& workspaceFiles);

    /**
     * @brief Collects all canonical file paths currently needed by open documents and modules.
     * @return Set of canonical file paths that must remain in memory.
     */
    [[nodiscard]] ankerl::unordered_dense::set<std::string> CollectWantedClosurePaths() const;

    /**
     * @brief Collects URIs of indexed closure files no longer required by any open document or module.
     * @param[in] wantedPaths Set of canonical file paths currently needed.
     * @return List of URIs to purge from cache.
     */
    [[nodiscard]] std::vector<std::string>
    CollectStaleClosureUris(const ankerl::unordered_dense::set<std::string>& wantedPaths) const;

    /**
     * @brief Collects names of symbols declared shared in other modules or outside the owning module.
     * @param[in] owning Owning module view to exclude from external shared searches.
     * @param[out] outShared Set of symbol names receiving externally shared declarations.
     */
    void CollectSharedSymbolsElsewhere(const ModuleView& owning,
                                       ankerl::unordered_dense::set<std::string>& outShared) const;

    /**
     * @brief The module scoping for one document, or nullopt when there is none to give.
     *
     * Nullopt when angelscript.modules is unset, and also when the document belongs to none of
     * the configured modules - a scratch file beside the project is not evidence about the
     * project, and rules that read this must stay as quiet for it as they were before.
     */
    [[nodiscard]] std::optional<angel_lsp::analysis::SemanticAnalysisRequest::ModuleContext>
    ModuleContextFor(const std::string& uriStr) const;

    std::vector<std::string> IncludeAllowedRoots() const;

    /**
     * @brief The suffix an unresolvable include may be retried with, or empty for none.
     *
     * One accessor rather than the rule repeated at each call site: resolution happens in the
     * include graph, in hover, in document links and in the rename fix-up, and a site that
     * forgot it would resolve differently from the rest for the same file.
     */
    /**
     * @brief Every file an `#include` in this workspace may name, as absolute paths.
     *
     * Answered from the include graph rather than by walking the disk: the graph was built by
     * exactly the walk that decides which files count - the script extension, the workspace
     * folders, the exclude globs - so a second walk would be both slower and free to disagree.
     */
    [[nodiscard]] std::vector<std::string> IncludableFiles() const
    {
        return m_includeGraph.AllFiles();
    }

    [[nodiscard]] std::string_view ImplicitIncludeExtension() const noexcept
    {
        return m_config.implicitIncludeExtension ? std::string_view(m_config.info.fileExtension) : std::string_view{};
    }

    lsp::requests::Initialize::Result HandleRequestsInitialized(lsp::requests::Initialize::Params&& params);
    void HandleNotificationsInitialized(lsp::notifications::Initialized::Params&& params);
    lsp::requests::Shutdown::Result HandleRequestsShutdown();
    void HandleNotificationsExit();
    void HandleNotificationsWorkspace_DidChangeConfiguration(
        lsp::notifications::Workspace_DidChangeConfiguration::Params&& params);

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
    void HandleNotificationsWorkspace_DidChangeWatchedFiles(
        lsp::notifications::Workspace_DidChangeWatchedFiles::Params&& params);

    /**
     * @brief Tracks folders added to or removed from a multi-root workspace.
     *
     * Both the include graph and the predefined-stub scan are scoped to the known roots, so a
     * folder added after initialize would otherwise stay invisible for the whole session.
     */
    void HandleNotificationsWorkspace_DidChangeWorkspaceFolders(
        lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params&& params);

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
    lsp::SemanticTokens ComputeAndCacheSemanticTokens(const std::string& uriStr, const std::string& text);

    /**
     * @brief Consumes `textDocument/willSave` notifications.
     *
     * The server has nothing it must do before a save: analysis is already kept current
     * on change notifications and document text is already held in memory. This handler
     * exists so the notification is consumed rather than dropped by a server that advertised
     * the capability.
     */
    void HandleNotificationsTextDocument_WillSave(lsp::notifications::TextDocument_WillSave::Params&& params);
    void HandleNotificationsTextDocument_DidSave(lsp::notifications::TextDocument_DidSave::Params&& params);

    /**
     * @brief Withdraws stale diagnostics or re-analyzes dependents when a document save alters interface.
     * @param[in] uriStr Document URI that was saved.
     * @param[in] interfaceChanged True if the public interface changed on save.
     */
    void HandleSavedInterfaceChange(const std::string& uriStr, bool interfaceChanged);
    void HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params&& params);
    void HandleNotificationsTextDocument_DidChange(lsp::notifications::TextDocument_DidChange::Params&& params);
    void HandleNotificationsTextDocument_DidClose(lsp::notifications::TextDocument_DidClose::Params&& params);
    /**
     * @brief Cancels any scan in flight and starts a fresh one on the workspace thread.
     *
     * The stop flag is shared by every generation of that thread, so the previous one has to be
     * joined before it is rearmed: clearing it while the old scan still runs would leave that
     * scan with a cancellation it can no longer see, and two scans reading the workspace at
     * once. Every start goes through here for that reason.
     */
    void StartWorkspaceScan();

    void ReadWorkspaceFiles(const angel_lsp::utils::StopFlag& stopToken);
    void RecoverFromScanFailure(const std::string& err);
    /**
     * @brief Reads a predefined stub off disk and indexes it.
     * @param forceReload Re-collect even when this server already owns the file, which is what
     *        a change reported by the file watcher needs.
     */
    void ParserPredefined(const std::string& filePath, angel_lsp::parser::AngelScriptParser& parser,
                          bool forceReload = false);
    /**
     * @brief Context parameters for recursive predefined stub loading and include parsing.
     */
    struct PredefinedLoadContext
    {
        angel_lsp::parser::AngelScriptParser& parser;
        bool forceReload = false;
        std::unordered_set<std::string>& visited;
    };

    /**
     * @brief Resolves and parses `#include` directives discovered inside a predefined stub.
     * @param[in] content Textual content of the predefined stub.
     * @param[in] filePath Path of the predefined stub on disk.
     * @param[in,out] ctx Predefined loading context.
     */
    void LoadPredefinedIncludes(const std::string& content, const std::string& filePath, PredefinedLoadContext& ctx);

    /**
     * @brief Recursively loads and indexes a predefined stub with cycle detection.
     * @param[in] filePath Path of the predefined stub on disk.
     * @param[in,out] parser Parser instance to reuse.
     * @param[in] forceReload True to re-index even if already owned.
     * @param[in,out] visited Set of visited normalized paths.
     */
    void ParserPredefinedInternal(const std::string& filePath, angel_lsp::parser::AngelScriptParser& parser,
                                  bool forceReload, std::unordered_set<std::string>& visited);

    /**
     * @brief Marks whether predefined stubs and engine profiles are ready for document analysis.
     * @param[in] ready True if predefined stubs are loaded and ready.
     */
    void SetPredefinedReady(bool ready);

    /**
     * @brief Waits up to timeout for predefined stubs to finish loading.
     * @param[in] timeout Maximum duration to wait.
     * @return True if predefined stubs are ready, false if timed out.
     */
    bool WaitForPredefinedReady(std::chrono::milliseconds timeout) const;

    /**
     * @brief Checks whether predefined stubs have finished loading.
     * @return True if predefined stubs are loaded.
     */
    [[nodiscard]] bool IsPredefinedReady() const noexcept;

    /**
     * @brief Loads the predefined stubs named by ServerConfig::predefinedFiles.
     *
     * Separate from the workspace scan because these are the stubs the scan cannot find: a
     * host application's declarations normally ship with the application, outside every
     * workspace folder.
     *
     * @param parser Parser to reuse across all of them.
     * @param stopToken Checked between files, so shutdown does not wait on the whole list.
     * @return The normalised paths actually loaded, for UnloadUnselectedPredefinedStubs. A
     *         configured stub stays loaded whatever the active selection is.
     */
    std::vector<std::string> LoadConfiguredPredefinedFiles(angel_lsp::parser::AngelScriptParser& parser,
                                                           const angel_lsp::utils::StopFlag& stopToken);

    /**
     * @brief Releases every loaded stub file whose path is not in @p wantedPaths.
     *
     * The counterpart, for stubs on disk, of the stale-profile purge in
     * LoadBuiltinEngineProfiles - and it was missing for years while that one existed.
     * ClaimPredefinedFile refuses a URI it has already seen, so without this a rescan could
     * only ever add: switching the active stub left the previous one's declarations resolving
     * in hover and completion with no diagnostic to say they no longer exist, and the
     * `#define`s it contributed still keeping `#if` blocks alive.
     *
     * Built-in profiles are untouched: their synthetic URIs have no filesystem path, so they
     * never enter m_predefinedUriByPath.
     *
     * @param wantedPaths Normalised paths that survive this scan.
     */
    void UnloadUnselectedPredefinedStubs(const std::vector<std::string>& wantedPaths);

    /**
     * @brief Re-reads the `#define`s of a stub open in the editor, and says whether they moved.
     *
     * A stub is this server's description of the host's engine setup, so its `#define FOO`
     * lines are what decide whether `#if FOO` is live code in every other open document. The
     * disk path records them (ParserPredefined); the editor path did not, so commenting a
     * `#define` out and saving changed the stub's symbols and left every `#if` exactly as it
     * was until the stub was reloaded by hand.
     *
     * @return True when the merged set changed, i.e. when the other open documents now compile
     *         differently and have to be re-analysed.
     */
    bool RefreshStubDefinedWords(const std::string& uriStr, const std::string& text);

    /**
     * @brief Loads built-in predefined stub profiles (e.g. Standard, SvenCoop, Urho3D, OpenXRay, OOTP).
     * @param parser Parser to reuse.
     * @param stopToken Checked between profiles for early exit on cancellation.
     */
    void LoadBuiltinEngineProfiles(angel_lsp::parser::AngelScriptParser& parser,
                                   const angel_lsp::utils::StopFlag& stopToken);

    /**
     * @brief Determines the engine profile to load based on configuration and workspace contents.
     * @param[in] stopToken Checked during auto-detection scan for early exit on cancellation.
     * @return Target engine profile kind, or std::nullopt if disabled, canceled, or kind is None.
     */
    [[nodiscard]] std::optional<angel_lsp::analysis::EngineProfileKind>
    ResolveTargetEngineProfile(const angel_lsp::utils::StopFlag& stopToken);

    /**
     * @brief Unloads synthetic profile URIs that are no longer in the active profile set.
     * @param[in] wantedProfiles Active profiles to preserve.
     */
    void UnloadStaleBuiltinEngineProfiles(const std::vector<angel_lsp::analysis::EngineProfileKind>& wantedProfiles);

    /**
     * @brief Loads a single engine profile stub into the symbol and scope tables.
     * @param[in] pKind Engine profile kind to load.
     * @param[in,out] parser Parser to reuse for AST generation.
     */
    void LoadEngineProfileStub(angel_lsp::analysis::EngineProfileKind pKind,
                               angel_lsp::parser::AngelScriptParser& parser);

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
     * @param customSymbolTable Optional custom or snapshot symbol table to use instead of m_symbolTable.
     * @return Request wired with the scope tree, type configuration and feature flags.
     */
    angel_lsp::analysis::SemanticAnalysisRequest
    BuildAnalysisRequest(const std::string& uriStr, const std::string& text, const TSTree* tree,
                         const angel_lsp::analysis::SymbolTable* customSymbolTable = nullptr) const;

    /**
     * @brief Parameters required to atomically commit analysis results for a document.
     */
    struct CommitAnalysisRequest
    {
        std::string uriStr;
        int version = -1;
        uint64_t generation = 0;
        uint64_t configRevision = 0;
        analysis::SymbolTable* staging = nullptr;
        std::shared_ptr<const analysis::Scope> scopeRoot;
        std::vector<analysis::CallSite> calls;
        std::vector<analysis::Diagnostic> diagnostics;
        std::string text;
    };

    /**
     * @brief Checks if analysis results are stale due to newer document edits, config changes, or cancellation.
     * @param[in] req Commit request to evaluate.
     * @return True if the results should be discarded, false if they are current.
     */
    [[nodiscard]] bool IsCommitAnalysisStale(const CommitAnalysisRequest& req) const;

    /**
     * @brief Atomically commits analysis results if and only if the document state is still current.
     *        Guarded by m_lifecycleMutex to serialize with document lifecycle mutations.
     * @param[in] req Parameters and analysis products to commit.
     * @return True if committed, false if rejected due to obsolescence or cancellation.
     */
    bool CommitAnalysisResults(CommitAnalysisRequest req);

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
    std::vector<angel_lsp::analysis::Diagnostic> ReplaceSymbolsFromTree(const std::string& uriStr,
                                                                        const std::string& text, TSTree* tree,
                                                                        bool* outInterfaceChanged = nullptr);

    /** @brief ReplaceSymbolsFromTree for a caller that has source text but no parsed tree. */
    std::vector<angel_lsp::analysis::Diagnostic> ReplaceSymbolsFromSource(const std::string& uriStr,
                                                                          const std::string& text,
                                                                          angel_lsp::parser::AngelScriptParser& parser);

    /**
     * @brief Parameters for scope collection and semantic analysis invocation.
     */
    struct CollectScopesRequest
    {
        std::string uriStr;
        std::string text;
        const TSTree* tree = nullptr;
        double* outScopeMs = nullptr;
        double* outCheckMs = nullptr;
        const analysis::NodeIndex* nodeIndex = nullptr;
    };

    /**
     * @brief Checks if scope analysis should be skipped (e.g. document too large or predefined).
     * @param[in] request Scope request to evaluate.
     * @return True if scope analysis should be skipped.
     */
    [[nodiscard]] bool ShouldSkipScopeAnalysis(const CollectScopesRequest& request);

    /**
     * @brief Collects the document's scopes and calls, analyses it, then publishes the scopes.
     * @param[in] request Bundled document parameters and timing destinations.
     * @return Diagnostics produced by semantic analysis.
     */
    std::vector<angel_lsp::analysis::Diagnostic> CollectScopesAndAnalyze(const CollectScopesRequest& request);

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
    bool ClaimPredefinedFile(const std::string& uriStr, bool forceReload = false);

    /**
     * @brief Removes everything one predefined document contributed to the index.
     *
     * There was no way to unload a stub at all, and three separate places wanted one: the
     * re-spelling branch of ClaimPredefinedFile, the file-deleted branch of
     * didChangeWatchedFiles, and switching engine profile. The first two had a copy each and
     * the third had nothing, which is why leaving a profile left its symbols behind - a rescan
     * only ever adds, because ClaimPredefinedFile refuses a URI it has already seen.
     *
     * @param uriStr The document to forget, as it was indexed. **By value on purpose**: this
     *        erases from m_predefinedUris and m_predefinedUriByPath, and the obvious thing to
     *        pass is a reference into one of them. Doing that dangles mid-call - it did, and
     *        the symptom was an unload that reported success on an empty URI while the real
     *        entry stayed. A copy makes the hazard unreachable rather than something to
     *        remember.
     * @return True when something was actually unloaded.
     * @note The caller must hold m_predefinedMutex.
     */
    bool UnloadPredefinedUri(std::string uriStr);

    /**
     * @brief True when two already-normalised paths name the same file.
     *
     * Case-insensitive on Windows, where the same file has many spellings and the one the
     * client sends need not match the one the directory walk produced. Everywhere else an
     * exact comparison, because everywhere else the case is part of the name.
     */
    static bool PathsAreSameFile(const std::string& a, const std::string& b);

    /**
     * @brief True when this stub's declarations may enter the workspace symbol table.
     *
     * The workspace scan honours the selection; opening a stub in the editor did not, and
     * didOpen/didChange/didSave each loaded whatever file was in front of them. So selecting
     * host_a and then opening host_b put host_b's types back in the table, where they resolved
     * happily - which is the opposite of what selecting one stub is for, and invisible, because
     * the symptom is a name that *works*.
     *
     * Empty means nothing has been decided: the scan has not finished, or every stub is being
     * merged. Both are "load it".
     */
    bool PredefinedStubContributes(const std::string& uriStr) const;

    /**
     * @brief Tells the user which stub is in force, once per scan.
     *
     * Two things are worth saying and neither was being said. A selection naming a file the
     * scan never saw is a typo, and staying silent about it costs the user every host type in
     * the workspace with nothing on screen to explain why. And several stubs with no selection
     * is the merge that this setting exists to avoid - legal, and the default, but the user
     * should know it is happening rather than discover it as a symbol resolving twice.
     *
     * @param discovered Canonical paths of every stub the walk found, in walk order.
     * @param activePath The canonical selection, or empty when there is none.
     */
    void ReportPredefinedSelection(const std::vector<std::string>& discovered, const std::string& activePath,
                                   const std::string& autoSelected, bool mergeAll);

    /**
     * @brief Canonical paths of every predefined stub the last workspace scan found.
     *
     * Kept so angelscript.listPredefinedStubs can answer from the message loop without walking
     * the filesystem again - and, more importantly, so the client never has to know what counts
     * as a stub. IsPredefinedFile matches `as.predefined` by name and `.as.predefined` by
     * suffix; a second copy of that rule in TypeScript would drift from this one exactly as the
     * three directory walks drifted from each other.
     *
     * Written by the workspace thread and read by the message loop, so it lives under the
     * runtime-config mutex with the rest of that traffic.
     */
    std::vector<std::string> m_discoveredPredefined;

    // The stub actually in force: the chosen one, or the one the scan picked when nothing was
    // chosen. Empty while every discovered stub is being merged, which is what the picker shows
    // as "all". Guarded by m_runtimeConfigMutex, like the list above it.
    std::string m_effectivePredefined;

    // Transitive #include paths discovered inside contributing predefined stubs.
    // Guarded by m_runtimeConfigMutex.
    ankerl::unordered_dense::set<std::string> m_predefinedTransitiveIncludes;

    /**
     * @brief True when the client asked for diagnostics with textDocument/diagnostic.
     *
     * The two delivery models are alternatives, not layers. A client that pulls also receives
     * anything pushed, and VS Code keeps each in its own collection, so every diagnostic
     * appeared twice - twice in the Problems panel, twice in a hover. Announcing both is right:
     * it is what lets one server serve either kind of client. Sending both is not.
     */
    bool m_clientPullsDiagnostics = false;

    /**
     * @brief True when the client announced support for workspace/diagnostic/refresh.
     */
    bool m_clientSupportsDiagnosticRefresh = false;

    /**
     * @brief Tells the client which line ranges the preprocessor drops, so it can dim them.
     *
     * A custom notification because LSP has none: `angelscript/inactiveRegions`, carrying the
     * document URI and a list of `{startLine, endLine}`. It is what the C++ extension does with
     * its own inactive regions, and for the reason this replaced painting them as comments -
     * a semantic token cannot reach the editor's bracket-pair colouring, which paints `(`, `{`
     * and `[` from its own feature and ignores both TextMate and semantic scopes. Dead code
     * therefore kept rainbow brackets no matter what tokens were emitted for it. A decoration
     * dims whatever is underneath, brackets included.
     */
    void PublishInactiveRegions(const std::string& uriStr, const std::string& text);
    /**
     * @brief Request bundle for publishing diagnostics for a document.
     */
    struct PublishDiagnosticsRequest
    {
        std::string uriStr;
        std::string text;
        std::vector<angel_lsp::analysis::Diagnostic> diagnostics;
        int version = -1;
        uint64_t generation = 0;
    };

    /**
     * @brief Checks whether diagnostic publication is stale due to generation or version mismatch.
     * @param[in] request Publish request to validate.
     * @return True if diagnostics should be discarded, false if current.
     */
    [[nodiscard]] bool IsPublishDiagnosticsStale(const PublishDiagnosticsRequest& request) const;

    /**
     * @brief Caches diagnostics snapshot for pull diagnostics requests.
     * @param[in] request Original publish request.
     * @param[in] protocolDiagnostics Converted protocol diagnostics items.
     */
    void CacheDiagnosticsSnapshot(const PublishDiagnosticsRequest& request,
                                  const std::vector<lsp::Diagnostic>& protocolDiagnostics);

    /**
     * @brief Sends diagnostic notification to the client or requests workspace refresh.
     * @param[in] params Publishing parameters.
     */
    void NotifyClientDiagnostics(lsp::notifications::TextDocument_PublishDiagnostics::Params params);

    /**
     * @brief Publishes diagnostics using bundled request parameters.
     * @param[in] request Publishing parameters and diagnostics.
     */
    void PublishDiagnostics(const PublishDiagnosticsRequest& request);

    /**
     * @brief Publishes diagnostics for an open document, fetching text from document store.
     * @param[in] uriStr Document URI.
     * @param[in] diagnostics Diagnostics to report.
     * @param[in] version Optional document version (-1 to query current version).
     */
    void PublishDiagnostics(const std::string& uriStr, const std::vector<angel_lsp::analysis::Diagnostic>& diagnostics,
                            int version = -1);

    /**
     * @brief Publishes diagnostics with explicit document text for line offset encoding.
     * @param[in] uriStr Document URI.
     * @param[in] text Document buffer text.
     * @param[in] diagnostics Diagnostics to report.
     * @param[in] version Optional document version (-1 to query current version).
     */
    void PublishDiagnostics(const std::string& uriStr, const std::string& text,
                            const std::vector<angel_lsp::analysis::Diagnostic>& diagnostics, int version = -1);

    /**
     * @brief Analyzer diagnostics as the client receives them: filtered, encoded, converted.
     *
     * The one place that translation happens. Both the push notification and the pull request
     * answer from this, which is what keeps them from drifting into two slightly different
     * answers for the same document.
     */
    std::vector<lsp::Diagnostic>
    ToProtocolDiagnostics(const std::string& text,
                          const std::vector<angel_lsp::analysis::Diagnostic>& diagnostics) const;

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
    lsp::requests::TextDocument_Diagnostic::Result
    HandleRequestsTextDocument_Diagnostic(lsp::requests::TextDocument_Diagnostic::Params&& params);

    /**
     * @brief Answers `workspace/diagnostic` for every document the server has already analysed.
     *
     * Deliberately not a workspace scan. It reports what is known - open documents and the
     * `#include` closure files pulled in on their behalf - rather than parsing the tree from
     * scratch, which on a 1,061-file corpus is minutes of work for a request the client sends
     * on a timer.
     */
    lsp::requests::Workspace_Diagnostic::Result
    HandleRequestsWorkspace_Diagnostic(lsp::requests::Workspace_Diagnostic::Params&& params);

    /**
     * @brief Answers `documentLink/resolve` by returning the link unchanged.
     *
     * Document links are resolved eagerly in `textDocument/documentLink` with their target URIs
     * and ranges fully populated. There is nothing left to compute; this handler exists so a
     * client that insists on the resolve round-trip gets a valid answer rather than MethodNotFound.
     */
    lsp::requests::DocumentLink_Resolve::Result
    HandleRequestsDocumentLink_Resolve(lsp::requests::DocumentLink_Resolve::Params&& params);

    /**
     * @brief Answers `inlayHint/resolve` by returning the hint unchanged.
     *
     * Inlay hints are computed complete with all labels, tooltips and parts during the
     * initial `textDocument/inlayHint` request. This handler returns the hint as-is so
     * clients that send a resolve request receive a valid response instead of MethodNotFound.
     */
    lsp::requests::InlayHint_Resolve::Result
    HandleRequestsInlayHint_Resolve(lsp::requests::InlayHint_Resolve::Params&& params);

    /**
     * @brief Answers `workspaceSymbol/resolve` by returning the symbol unchanged.
     *
     * Workspace symbols already carry their full location and container name when collected
     * for `workspace/symbol`. Returning the symbol unchanged ensures clients requesting
     * symbol resolution get a valid response instead of MethodNotFound.
     */
    lsp::requests::WorkspaceSymbol_Resolve::Result
    HandleRequestsWorkspaceSymbol_Resolve(lsp::requests::WorkspaceSymbol_Resolve::Params&& params);

    /**
     * @brief Answers `textDocument/rangesFormatting` by formatting each requested range.
     *
     * Clients supporting LSP 3.18 format multiple disparate selection ranges in a single
     * round-trip instead of serialising N requests. Range formatting operations are executed
     * across each range and the resulting edits are merged and encoded into the client's coordinate space.
     */
    lsp::requests::TextDocument_RangesFormatting::Result
    HandleRequestsTextDocument_RangesFormatting(lsp::requests::TextDocument_RangesFormatting::Params&& params);

    /**
     * @brief Answers `textDocument/willSaveWaitUntil` by formatting the document before save.
     *
     * Automatically formats the document on save, but only when manually triggered by the user
     * (`TextDocumentSaveReason::Manual`). Saves triggered by an autosave timer or focus loss
     * are ignored and return an empty edit array because an autosave timer would rewrite the
     * user's file while they are still actively typing in it.
     */
    lsp::requests::TextDocument_WillSaveWaitUntil::Result
    HandleRequestsTextDocument_WillSaveWaitUntil(lsp::requests::TextDocument_WillSaveWaitUntil::Params&& params);

    /**
     * @brief Answers `workspace/executeCommand` requests.
     *
     * Supports explicit workspace rescans (`angelscript.rescanWorkspace`) so external commands
     * or editor extensions can force an immediate rescan of all search paths and workspace
     * folders on demand. Unknown commands are rejected with InvalidParams.
     */
    lsp::requests::Workspace_ExecuteCommand::Result
    HandleRequestsWorkspace_ExecuteCommand(lsp::requests::Workspace_ExecuteCommand::Params&& params);

    /**
     * @brief Generates virtual mixin document content for an angelscript-virtual URI.
     * @param uri The synthetic virtual URI (e.g. angelscript-virtual://HostClass/MixinName.as).
     * @return Synthesized AngelScript document text.
     */
    std::string GenerateVirtualMixinDocument(std::string_view uri);

    /**
     * @brief Handles custom JSON-RPC request for angelscript/virtualDocumentContent.
     * @param params JSON parameter containing the URI.
     * @return JSON object with { "content": "..." }.
     */
    lsp::json::Value HandleRequestsVirtualDocumentContent(lsp::json::Value&& params);

    /** @brief Handles textDocument/prepareCallHierarchy request. */
    lsp::requests::TextDocument_PrepareCallHierarchy::Result
    HandleRequestsTextDocument_PrepareCallHierarchy(lsp::requests::TextDocument_PrepareCallHierarchy::Params&& req);

    /** @brief Handles callHierarchy/incomingCalls request. */
    lsp::requests::CallHierarchy_IncomingCalls::Result
    HandleRequestsCallHierarchy_IncomingCalls(lsp::requests::CallHierarchy_IncomingCalls::Params&& req);

    /** @brief Handles callHierarchy/outgoingCalls request. */
    lsp::requests::CallHierarchy_OutgoingCalls::Result
    HandleRequestsCallHierarchy_OutgoingCalls(lsp::requests::CallHierarchy_OutgoingCalls::Params&& req);

    /** @brief Handles textDocument/prepareTypeHierarchy request. */
    lsp::requests::TextDocument_PrepareTypeHierarchy::Result
    HandleRequestsTextDocument_PrepareTypeHierarchy(lsp::requests::TextDocument_PrepareTypeHierarchy::Params&& req);

    /** @brief Handles typeHierarchy/supertypes request. */
    lsp::requests::TypeHierarchy_Supertypes::Result
    HandleRequestsTypeHierarchy_Supertypes(lsp::requests::TypeHierarchy_Supertypes::Params&& req);

    /** @brief Handles typeHierarchy/subtypes request. */
    lsp::requests::TypeHierarchy_Subtypes::Result
    HandleRequestsTypeHierarchy_Subtypes(lsp::requests::TypeHierarchy_Subtypes::Params&& req);

    /** @brief Handles textDocument/linkedEditingRange request. */
    lsp::requests::TextDocument_LinkedEditingRange::Result
    HandleRequestsTextDocument_LinkedEditingRange(lsp::requests::TextDocument_LinkedEditingRange::Params&& req);

    /** @brief Handles textDocument/selectionRange request. */
    lsp::requests::TextDocument_SelectionRange::Result
    HandleRequestsTextDocument_SelectionRange(lsp::requests::TextDocument_SelectionRange::Params&& req);

    /** @brief Handles textDocument/semanticTokens/full request. */
    lsp::requests::TextDocument_SemanticTokens_Full::Result
    HandleRequestsTextDocument_SemanticTokens_Full(lsp::requests::TextDocument_SemanticTokens_Full::Params&& req);

    /** @brief Handles textDocument/semanticTokens/full/delta request. */
    lsp::requests::TextDocument_SemanticTokens_Full_Delta::Result HandleRequestsTextDocument_SemanticTokens_Full_Delta(
        lsp::requests::TextDocument_SemanticTokens_Full_Delta::Params&& req);

    /** @brief Handles textDocument/semanticTokens/range request. */
    lsp::requests::TextDocument_SemanticTokens_Range::Result
    HandleRequestsTextDocument_SemanticTokens_Range(lsp::requests::TextDocument_SemanticTokens_Range::Params&& req);

    /** @brief Handles textDocument/foldingRange request. */
    lsp::requests::TextDocument_FoldingRange::Result
    HandleRequestsTextDocument_FoldingRange(lsp::requests::TextDocument_FoldingRange::Params&& req);

    /** @brief Handles textDocument/inlayHint request. */
    lsp::requests::TextDocument_InlayHint::Result
    HandleRequestsTextDocument_InlayHint(lsp::requests::TextDocument_InlayHint::Params&& req);

    /** @brief Handles workspace/textDocumentContent request. */
    lsp::requests::Workspace_TextDocumentContent::Result
    HandleRequestsWorkspace_TextDocumentContent(lsp::requests::Workspace_TextDocumentContent::Params&& req);

    /** @brief Handles workspace/symbol request. */
    lsp::requests::Workspace_Symbol::Result
    HandleRequestsWorkspace_Symbol(lsp::requests::Workspace_Symbol::Params&& req);

    void RegisterLifecycleHandlers();
    void RegisterFileEventHandlers();
    void RegisterDocumentSyncHandlers();
    void RegisterWorkspaceActionHandlers();
    void RegisterNavigationHandlers();
    void RegisterEditingHandlers();
    void RegisterFormattingAndSymbolHandlers();

    /** @brief Handles textDocument/hover request. */
    lsp::requests::TextDocument_Hover::Result
    HandleRequestsTextDocument_Hover(lsp::requests::TextDocument_Hover::Params&& req);

    /** @brief Handles textDocument/definition request. */
    lsp::requests::TextDocument_Definition::Result
    HandleRequestsTextDocument_Definition(lsp::requests::TextDocument_Definition::Params&& req);

    /** @brief Handles textDocument/moniker request. */
    lsp::requests::TextDocument_Moniker::Result
    HandleRequestsTextDocument_Moniker(lsp::requests::TextDocument_Moniker::Params&& req);

    /** @brief Handles textDocument/declaration request. */
    lsp::requests::TextDocument_Declaration::Result
    HandleRequestsTextDocument_Declaration(lsp::requests::TextDocument_Declaration::Params&& req);

    /** @brief Handles textDocument/implementation request. */
    lsp::requests::TextDocument_Implementation::Result
    HandleRequestsTextDocument_Implementation(lsp::requests::TextDocument_Implementation::Params&& req);

    /** @brief Handles textDocument/typeDefinition request. */
    lsp::requests::TextDocument_TypeDefinition::Result
    HandleRequestsTextDocument_TypeDefinition(lsp::requests::TextDocument_TypeDefinition::Params&& req);

    /** @brief Handles textDocument/references request. */
    lsp::requests::TextDocument_References::Result
    HandleRequestsTextDocument_References(lsp::requests::TextDocument_References::Params&& req);

    /** @brief Handles textDocument/documentHighlight request. */
    lsp::requests::TextDocument_DocumentHighlight::Result
    HandleRequestsTextDocument_DocumentHighlight(lsp::requests::TextDocument_DocumentHighlight::Params&& req);

    /** @brief Handles textDocument/completion request. */
    lsp::requests::TextDocument_Completion::Result
    HandleRequestsTextDocument_Completion(lsp::requests::TextDocument_Completion::Params&& req);

    /** @brief Handles completionItem/resolve request. */
    lsp::requests::CompletionItem_Resolve::Result
    HandleRequestsCompletionItem_Resolve(lsp::requests::CompletionItem_Resolve::Params&& req);

    /** @brief Handles textDocument/signatureHelp request. */
    lsp::requests::TextDocument_SignatureHelp::Result
    HandleRequestsTextDocument_SignatureHelp(lsp::requests::TextDocument_SignatureHelp::Params&& req);

    /** @brief Handles textDocument/prepareRename request. */
    lsp::requests::TextDocument_PrepareRename::Result
    HandleRequestsTextDocument_PrepareRename(lsp::requests::TextDocument_PrepareRename::Params&& req);

    /** @brief Handles textDocument/rename request. */
    lsp::requests::TextDocument_Rename::Result
    HandleRequestsTextDocument_Rename(lsp::requests::TextDocument_Rename::Params&& req);

    /** @brief Handles textDocument/codeAction request. */
    lsp::requests::TextDocument_CodeAction::Result
    HandleRequestsTextDocument_CodeAction(lsp::requests::TextDocument_CodeAction::Params&& req);

    /** @brief Handles codeAction/resolve request. */
    lsp::requests::CodeAction_Resolve::Result
    HandleRequestsCodeAction_Resolve(lsp::requests::CodeAction_Resolve::Params&& req);

    /** @brief Handles textDocument/documentSymbol request. */
    lsp::requests::TextDocument_DocumentSymbol::Result
    HandleRequestsTextDocument_DocumentSymbol(lsp::requests::TextDocument_DocumentSymbol::Params&& req);

    /** @brief Handles textDocument/formatting request. */
    lsp::requests::TextDocument_Formatting::Result
    HandleRequestsTextDocument_Formatting(lsp::requests::TextDocument_Formatting::Params&& req);

    /** @brief Handles textDocument/rangeFormatting request. */
    lsp::requests::TextDocument_RangeFormatting::Result
    HandleRequestsTextDocument_RangeFormatting(lsp::requests::TextDocument_RangeFormatting::Params&& req);

    /** @brief Handles textDocument/onTypeFormatting request. */
    lsp::requests::TextDocument_OnTypeFormatting::Result
    HandleRequestsTextDocument_OnTypeFormatting(lsp::requests::TextDocument_OnTypeFormatting::Params&& req);

    /** @brief Handles textDocument/documentLink request. */
    lsp::requests::TextDocument_DocumentLink::Result
    HandleRequestsTextDocument_DocumentLink(lsp::requests::TextDocument_DocumentLink::Params&& req);

    /** @brief Handles textDocument/codeLens request. */
    lsp::requests::TextDocument_CodeLens::Result
    HandleRequestsTextDocument_CodeLens(lsp::requests::TextDocument_CodeLens::Params&& req);

    /** @brief Handles codeLens/resolve request. */
    lsp::requests::CodeLens_Resolve::Result
    HandleRequestsCodeLens_Resolve(lsp::requests::CodeLens_Resolve::Params&& req);

    /**
     * @brief Bundles parameters for processing an open predefined stub file.
     */
    struct DidOpenPredefinedRequest
    {
        const std::string& uriStr;
        const std::string& text;
        int version = 0;
        TSTree* tree = nullptr;
        double parseMs = 0.0;
        const utils::HighResTimer& totalTimer;
    };

    /**
     * @brief Processes saving a predefined file stub.
     * @param[in] uriStr Document URI key.
     * @param[in] text Document contents.
     */
    void DidSavePredefinedFile(const std::string& uriStr, const std::string& text);

    /**
     * @brief Reanalyzes dependent documents when an interface change is detected on save.
     * @param[in] uriStr Saved document URI key.
     */
    void ReanalyzeDependentsOnSave(const std::string& uriStr);

    /**
     * @brief Processes opening of a predefined file stub and publishes empty diagnostics.
     * @param[in] req Request containing URI, document text, version, parse tree and timers.
     */
    void DidOpenPredefinedFile(const DidOpenPredefinedRequest& req);

    /**
     * @brief Applies an incremental or full content change event to a document buffer and parse tree.
     * @param[in,out] buffer Target text buffer to modify.
     * @param[in,out] workingTree Target AST tree to update incrementally.
     * @param[in] change LSP change event.
     * @param[in] isPredefined True if the document is a predefined stub.
     */
    void ApplyContentChange(std::string& buffer, document::TreePtr& workingTree,
                            const lsp::TextDocumentContentChangeEvent& change, bool isPredefined);

    /**
     * @brief Restores a closed document as an on-disk closure file if claimed by an active module.
     * @param[in] uriStr Document URI key.
     * @param[in] path Canonical filesystem path.
     * @return True if file was restored and re-analyzed as a module closure file.
     */
    bool RestoreClosedModuleFile(const std::string& uriStr, const std::string& path);

    /**
     * @brief Extracts initial workspace folder paths from initialization parameters.
     * @param[in] params LSP initialize request parameters.
     */
    void ExtractInitialWorkspaceRoots(const lsp::requests::Initialize::Params& params);

    /**
     * @brief Parses and applies engine configuration from initialization options.
     * @param[in] initOpts Optional initialization options from client.
     */
    void ApplyEngineInitializationOptions(const std::optional<lsp::LSPAny>& initOpts);

    /**
     * @brief Negotiates protocol capabilities supported by the client.
     * @param[in] capabilities Client capabilities payload.
     */
    void NegotiateClientCapabilities(const lsp::ClientCapabilities& capabilities);

    /**
     * @brief Configures navigation and editing provider capabilities.
     * @param[in,out] caps Server capabilities structure to populate.
     */
    void ConfigureNavigationAndEditingCapabilities(lsp::ServerCapabilities& caps) const;

    /**
     * @brief Configures symbol, completion, and semantic token capabilities.
     * @param[in,out] caps Server capabilities structure to populate.
     */
    void ConfigureEditingAndSymbolCapabilities(lsp::ServerCapabilities& caps) const;

    /**
     * @brief Configures formatting, code action, and diagnostics capabilities.
     * @param[in,out] caps Server capabilities structure to populate.
     */
    void ConfigureFormattingAndDiagnosticCapabilities(lsp::ServerCapabilities& caps) const;

    /**
     * @brief Configures workspace folders, file operations, and command capabilities.
     * @param[in,out] caps Server capabilities structure to populate.
     */
    void ConfigureWorkspaceCapabilities(lsp::ServerCapabilities& caps) const;

    /**
     * @brief Constructs full server capabilities responding to initialization request.
     * @return Populated server capabilities object.
     */
    lsp::ServerCapabilities BuildServerCapabilities() const;

    /**
     * @brief Updates format brace style settings from the workspace configuration.
     * @param[in] section Configuration object.
     */
    void UpdateFormatConfiguration(const lsp::LSPObject& section);

    /**
     * @brief Updates AngelScript engine settings from the workspace configuration.
     * @param[in] section Configuration object.
     * @return True if any engine setting was changed.
     */
    bool UpdateEngineConfiguration(const lsp::LSPObject& section);

    /**
     * @brief Updates feature toggle flags from the workspace configuration.
     * @param[in] section Configuration object.
     */
    void UpdateFeatureConfiguration(const lsp::LSPObject& section);

    /**
     * @brief Updates module definitions from the workspace configuration.
     * @param[in] section Configuration object.
     * @return True if module list changed and requires a workspace rescan.
     */
    bool UpdateModulesConfiguration(const lsp::LSPObject& section);

    /**
     * @brief Updates include extension settings from configuration.
     * @param[in] section Configuration object.
     * @return True if include settings changed requiring a rescan.
     */
    bool UpdateIncludeConfiguration(const lsp::LSPObject& section);

    /**
     * @brief Updates active predefined stub and engine profile settings from configuration.
     * @param[in] section Configuration object.
     * @return True if predefined or engine profile changed requiring a rescan.
     */
    bool UpdatePredefinedAndProfileConfiguration(const lsp::LSPObject& section);

    /**
     * @brief Updates include search directories from configuration.
     * @param[in] section Configuration object.
     * @return True if search directories changed requiring a rescan.
     */
    bool UpdateSearchDirectoriesConfiguration(const lsp::LSPObject& section);

    /**
     * @brief Formats a predefined stub file requested via workspace/executeCommand.
     * @param[in] args Command arguments containing target stub URI.
     * @return Result value containing format status and modified text.
     */
    lsp::requests::Workspace_ExecuteCommand::Result
    ExecuteFormatPredefinedStub(const std::optional<lsp::Array<lsp::LSPAny>>& args);

    /**
     * @brief Lists available predefined stubs requested via workspace/executeCommand.
     * @return Result value containing discovered stubs list.
     */
    lsp::requests::Workspace_ExecuteCommand::Result ExecuteListPredefinedStubs() const;

    /**
     * @brief Bundles discovered file collections from a unified workspace filesystem walk.
     */
    struct WorkspaceFilesWalkResult
    {
        std::vector<std::string> allScriptFiles;
        std::vector<std::string> allFileNames;
        std::vector<std::string> discoveredStubPaths;
    };

    /**
     * @brief Collects all script and stub files across workspace roots in a single pass.
     * @param[in] roots Workspace root directory paths.
     * @param[in] stopToken Cancellation token.
     * @param[out] outFiles Container to receive discovered file paths.
     * @return True if walk completed without cancellation.
     */
    bool CollectWorkspaceFiles(const std::vector<std::string>& roots, const angel_lsp::utils::StopFlag& stopToken,
                               WorkspaceFilesWalkResult& outFiles);

    /**
     * @brief Loads engine profiles and configured stubs, then processes discovered stubs.
     * @param[in] discoveredStubPaths Paths of stubs found during filesystem walk.
     * @param[in] stopToken Cancellation token.
     * @param[in,out] parser Parser instance for reading stub contents.
     * @return True if completed without cancellation.
     */
    bool LoadAndProcessPredefinedStubs(const std::vector<std::string>& discoveredStubPaths,
                                       const angel_lsp::utils::StopFlag& stopToken,
                                       angel_lsp::parser::AngelScriptParser& parser);

    /**
     * @brief Discovers, parses, and unloads predefined stubs according to configuration.
     * @param[in] discoveredStubPaths Paths of stubs found during filesystem walk.
     * @param[in] configuredPaths Paths of explicitly configured stubs.
     * @param[in] stopToken Cancellation token.
     * @param[in,out] parser Parser instance for reading stub contents.
     */
    void ProcessDiscoveredPredefinedStubs(const std::vector<std::string>& discoveredStubPaths,
                                          const std::vector<std::string>& configuredPaths,
                                          const angel_lsp::utils::StopFlag& stopToken,
                                          angel_lsp::parser::AngelScriptParser& parser);

    /**
     * @brief Builds include graph and indexes configured modules during workspace scan.
     * @param[in] allScriptFiles Discovered script files.
     * @param[in] roots Workspace root directory paths.
     * @param[in] stopToken Cancellation token.
     * @param[in,out] parser Parser instance for indexing module files.
     */
    void BuildIncludeGraphAndModules(const std::vector<std::string>& allScriptFiles,
                                     const std::vector<std::string>& roots, const angel_lsp::utils::StopFlag& stopToken,
                                     angel_lsp::parser::AngelScriptParser& parser);

    /**
     * @brief Handles a deleted file event reported by the watched files notification.
     * @param[in] path Canonical filesystem path of deleted file.
     * @param[in] uriStr Document URI key of deleted file.
     * @param[in] isPredefined True if file is a predefined stub.
     * @param[in,out] stubSelectionInvalidated Set to true if the deleted file was the active stub.
     * @return True if the include graph or symbols changed.
     */
    bool HandleWatchedFileDeleted(const std::string& path, const std::string& uriStr, bool isPredefined,
                                  bool& stubSelectionInvalidated);

    /**
     * @brief Handles a changed or created file event reported by the watched files notification.
     * @param[in] path Canonical filesystem path of changed file.
     * @param[in] isPredefined True if file is a predefined stub.
     * @param[in,out] parser Parser instance for reading changed stub or closure.
     * @return True if the include graph or symbols changed.
     */
    bool HandleWatchedFileChanged(const std::string& path, bool isPredefined,
                                  angel_lsp::parser::AngelScriptParser& parser);

    /**
     * @brief Replaces an active predefined stub that was deleted on disk with the next candidate.
     * @param[in,out] parser Parser instance to index replacement stub.
     */
    void ReplaceDeletedPredefinedStub(angel_lsp::parser::AngelScriptParser& parser);

    /**
     * @brief Refreshes include directives for all open documents after filesystem changes.
     */
    void RefreshOpenDocumentIncludes();

    /**
     * @brief Full text of an indexed document, or nullptr when the server holds none.
     *
     * Needed by every position conversion: translating a Tree-sitter byte column into the
     * client's encoding requires the bytes of that line. Results pointing at a document the
     * server has no text for are handed back in byte columns unchanged - wrong only on
     * non-ASCII lines, and strictly better than dropping the result.
     */
    std::shared_ptr<const std::string> FindDocumentText(const std::string& uri) const;

    /**
     * @brief Canonical filesystem path behind a document URI, or empty for a non-file URI.
     *
     * The include graph is keyed by path while the rest of the server is keyed by URI, and the
     * same file can legitimately arrive spelled several ways (percent-encoded drive letters,
     * mixed separators). Everything crossing between the two goes through this.
     */
    static std::string CanonicalPathFromUri(const std::string& uriStr);

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
    static std::string DocumentKey(const std::string& uriStr);

    /**
     * @brief The three things nearly every document request starts by working out.
     *
     * `text` points into m_openDocuments and is valid only while the message loop owns it,
     * which is the whole lifetime of a synchronous handler. `tree` is null for a document that
     * has not been parsed yet, which every caller already had to tolerate.
     */
    struct OpenDocument
    {
        std::string uri;
        const std::string* text = nullptr;
        TSTree* tree = nullptr;
        std::shared_ptr<const document::Document> docHandle;
        std::shared_ptr<const std::string> predefinedHandle;
    };

    /**
     * @brief Resolves an incoming document URI to the open document behind it.
     *
     * Twenty-eight handlers opened with the same eight lines: canonicalise the URI, look it up
     * in m_openDocuments, answer null when it is not there, then fetch the tree. Written once
     * so a change to any of those four steps - a new encoding rule, a cancellation check - is
     * one edit rather than twenty-eight.
     *
     * @return The document, or nullopt when nothing by that URI is open. A request naming a
     *         document nobody opened is answered with null rather than an error: the client is
     *         allowed to race a close against a request it already sent.
     */
    std::optional<OpenDocument> LookupOpenDocument(const std::string& uriStr);

    void HandleNotificationsWorkspace_DidRenameFiles(lsp::notifications::Workspace_DidRenameFiles::Params&& params);

    /**
     * @brief Stops the workspace scan when the user dismisses its progress notification.
     *
     * The scan already polls a stop flag on every file so a folder change or a shutdown can
     * interrupt it; this is that flag reached from the other side. Compared against the token
     * the scan announced, because a client may run several progress operations at once.
     */
    void HandleNotificationsWindow_WorkDoneProgress_Cancel(
        lsp::notifications::Window_WorkDoneProgress_Cancel::Params&& params);

    /**
     * @brief Raises or lowers log verbosity without a restart.
     *
     * The protocol's three trace values are mapped onto this server's levels rather than parsed
     * into them - `off` still reports real failures, because a log that hides errors is not
     * what a user asking for less noise meant.
     */
    void HandleNotificationsSetTrace(lsp::notifications::SetTrace::Params&& params);

    /**
     * @brief Re-resolves open documents' `#include` directives when a script file appears.
     *
     * Deliberately the reverse direction from what it first looks like. The graph has no edge
     * INTO a file that did not exist when the edge was built, so asking who includes the new
     * file answers nobody - which is exactly the case this notification exists for. What
     * changed is that an OPEN document's directive now names something real.
     */
    void HandleNotificationsWorkspace_DidCreateFiles(lsp::notifications::Workspace_DidCreateFiles::Params&& params);
    void HandleNotificationsWorkspace_DidDeleteFiles(lsp::notifications::Workspace_DidDeleteFiles::Params&& params);

    /**
     * @brief One file's `#include` lines rewritten to a renamed target, or nullopt.
     *
     * Returns an edit for the editor to apply rather than writing the file: the change belongs
     * on the undo stack beside the rename that caused it, and a server editing files behind the
     * user's back is not something they can undo.
     */
    std::optional<lsp::TextDocumentEdit> BuildIncludeRewrite(const std::string& includerPath,
                                                             const std::string& oldTargetPath,
                                                             const std::string& newTargetPath);

    /** @brief Recomputes every open document's module closure and re-diagnoses it. */
    void ReanalyseOpenDocuments();

    /**
     * @brief Document URI for a filesystem path, in the spelling used for synthesised entries.
     */
    static std::string UriFromPath(const std::string& path);

    /** @brief The brace style the formatting handlers should use right now. */
    features::BraceStyle CurrentBraceStyle() const
    {
        return m_formatBraceStyleKR.load(std::memory_order_relaxed) ? features::BraceStyle::KAndR
                                                                    : features::BraceStyle::Allman;
    }

    /**
     * @brief Computes the module closure paths for an open document path.
     *
     * Considers entry-point forward scoping if configured, otherwise falls back
     * to full module closure, and appends any forced includes and their closures.
     *
     * @param[in] openPath Canonical path of the opened file.
     * @return Vector of canonical file paths belonging to the module closure.
     */
    std::vector<std::string> ComputeModuleClosure(const std::string& openPath) const;

    /**
     * @brief Indexes every other file in the opened document's #include module.
     *
     * AngelScript modules are composed textually, so a file can use declarations from the file
     * that includes it. Opening one member of a module therefore has to index the whole module,
     * upwards as well as downwards - see WorkspaceIncludeGraph::GetModuleClosure.
     */
    void IndexModuleClosure(const std::string& openUriStr);

    /**
     * @brief Drops the closure files an open document pulled in, unless another open document
     *        still needs them.
     */
    void ReleaseModuleClosure(const std::string& openUriStr);

    /**
     * @brief Parses one closure file and adds its symbols and scopes to the index.
     */
    void IndexClosureFile(const std::string& path, angel_lsp::parser::AngelScriptParser& parser);

    /**
     * @brief Removes a closure file's symbols, scopes and cached text.
     */
    void PurgeClosureFile(const std::string& uriStr);

    /**
     * @brief Appends a warning for every #include in the document that resolves to no file.
     *
     * Lives here rather than in SemanticAnalyzer because resolution depends on the configured
     * search directories, which are server state rather than anything the AST knows about.
     */
    void AppendIncludeDiagnostics(const std::string& uriStr, const std::string& text,
                                  std::vector<angel_lsp::analysis::Diagnostic>& diagnostics) const;

    /**
     * @brief Queues a document for re-analysis once the user stops typing.
     *
     * Funnel for all background analysis: callers pass raw mirror text, and ScheduleAnalysis
     * applies AnalysisTextFor before queueing so that stub rewrites happen exactly once and
     * callers need not know about them. Coalescing is by URI, so a burst of keystrokes collapses
     * into a single run against the latest text.
     */
    // `force` says the answer can differ even though the bytes did not - the symbol table
    // moved, not the buffer. Without it the dedupe drops the request as a duplicate of the
    // analysis whose answer is exactly the one being replaced.
    /**
     * @brief Request bundle for queueing background or immediate document analysis.
     */
    struct ScheduleAnalysisRequest
    {
        std::string uriStr;
        std::string text;
        bool force = false;
        angel_lsp::document::TreePtr tree = angel_lsp::document::MakeTreePtr(nullptr);
        int version = -1;
        uint64_t generation = 0;
    };

    /**
     * @brief Queues a document for re-analysis once debounce expires.
     * @param[in] req Analysis scheduling parameters.
     */
    void ScheduleAnalysis(ScheduleAnalysisRequest req);

    /**
     * @brief Convenience overload for queueing document re-analysis with default options.
     * @param[in] uriStr Document URI.
     * @param[in] text Document text.
     * @param[in] force True to force re-analysis even if content is unchanged.
     */
    void ScheduleAnalysis(const std::string& uriStr, const std::string& text, bool force = false);

    /**
     * @brief Queues a document for immediate re-analysis without debounce delay.
     * @param[in] req Immediate analysis scheduling parameters.
     */
    void ScheduleAnalysisImmediate(ScheduleAnalysisRequest req);

    /**
     * @brief Convenience overload for immediate document re-analysis with default options.
     * @param[in] uriStr Document URI.
     * @param[in] text Document text.
     * @param[in] force True to force re-analysis even if content is unchanged.
     */
    void ScheduleAnalysisImmediate(const std::string& uriStr, const std::string& text, bool force = false);

    /**
     * @brief Request bundle for analyzing a document in the worker thread.
     */
    struct AnalyzeDocumentRequest
    {
        std::string uriStr;
        std::string text;
        angel_lsp::parser::AngelScriptParser& parser;
        angel_lsp::document::TreePtr treeCopy = angel_lsp::document::MakeTreePtr(nullptr);
        int version = -1;
        uint64_t generation = 0;
        uint64_t configRevision = 0;
    };

    /**
     * @brief Checks if document analysis in worker thread has become stale before starting.
     * @param[in] req Analysis request to validate.
     * @return True if stale or cancelled.
     */
    [[nodiscard]] bool IsAnalyzeDocumentStale(const AnalyzeDocumentRequest& req) const;

    /**
     * @brief Analyzes a predefined stub document and commits its symbols and defined words.
     * @param[in] req Analysis request bundle.
     * @param[in] totalTimer High-resolution timer tracking total analysis time.
     */
    void AnalyzePredefinedDocument(AnalyzeDocumentRequest req, const utils::HighResTimer& totalTimer);

    /**
     * @brief Analyzes a normal AngelScript document and commits its symbols, scopes, and diagnostics.
     * @param[in] req Analysis request bundle.
     * @param[in] totalTimer High-resolution timer tracking total analysis time.
     */
    void AnalyzeNormalDocument(AnalyzeDocumentRequest req, const utils::HighResTimer& totalTimer);

    /**
     * @brief Rebuilds symbols, scopes and diagnostics for one document and publishes them.
     * @param[in] req Bundled document parameters and parser references.
     */
    void AnalyzeDocument(AnalyzeDocumentRequest req);

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
    void EncodeIn(std::string_view text, lsp::Range& range) const;
    void EncodeIn(std::string_view text, lsp::Hover& hover) const;
    void EncodeIn(std::string_view text, std::vector<lsp::TextEdit>& edits) const;
    void EncodeIn(std::string_view text, std::vector<lsp::DocumentHighlight>& highlights) const;
    void EncodeIn(std::string_view text, std::vector<lsp::FoldingRange>& ranges) const;
    void EncodeIn(std::string_view text, std::vector<lsp::DocumentLink>& links) const;
    void EncodeIn(std::string_view text, std::vector<lsp::InlayHint>& hints) const;
    void EncodeIn(std::string_view text, std::vector<lsp::DocumentSymbol>& symbols) const;
    void EncodeIn(std::string_view text, std::vector<lsp::CodeLens>& lenses) const;
    void EncodeIn(std::string_view text, lsp::PrepareRenameResult& result) const;
    void EncodeAcrossDocuments(std::vector<lsp::Location>& locations) const;
    void EncodeAcrossDocuments(std::vector<lsp::SymbolInformation>& symbols) const;
    void EncodeAcrossDocuments(lsp::WorkspaceEdit& edit) const;
    void EncodeAcrossDocuments(std::vector<lsp::CodeAction>& actions) const;

    /**
     * @brief Converts the two ranges a hierarchy item carries, against its own document.
     *
     * A hierarchy walks across files the user never opened, so the item's text is looked up by
     * URI like every other AcrossDocuments conversion. Templated because CallHierarchyItem and
     * TypeHierarchyItem share the shape and nothing else - the protocol declares them as two
     * unrelated structs.
     */
    template <typename ItemT> void EncodeItemRanges(ItemT& item) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        if (auto text = FindDocumentText(item.uri.toString()))
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
    void EncodeRangesIn(const std::string& uri, std::vector<lsp::Range>& ranges) const;

    /**
     * @brief Null-safe logging forwarding helpers.
     */
    void LogInfo(std::string_view message) const
    {
        if (m_logger)
        {
            m_logger->LogInfo(message);
        }
    }

    void LogWarning(std::string_view message) const
    {
        if (m_logger)
        {
            m_logger->LogWarning(message);
        }
    }

    void LogError(std::string_view message) const
    {
        if (m_logger)
        {
            m_logger->LogError(message);
        }
    }

    void LogDebug(std::string_view message) const
    {
        if (m_logger)
        {
            m_logger->LogDebug(message);
        }
    }

    void LogTrace(std::string_view message) const
    {
        if (m_logger)
        {
            m_logger->LogTrace(message);
        }
    }
};
} // namespace angel_lsp