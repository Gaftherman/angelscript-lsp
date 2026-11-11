#pragma once

#include "analysis/SemanticAnalyzer.h"
#include "analysis/index/CallGraph.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/types/EngineProfiles.h"
#include "config/ServerConfig.h"
#include "document/Document.h"
#include "features/definition/DefinitionHandler.h"
#include "features/formatting/FormattingHandler.h"
#include "i18n/i18n.h"
#include "lsp/pipeline/DiagnosticsPipeline.h"
#include "lsp/pipeline/LspSessionCoordinator.h"
#include "lsp/services/ServerAnalysisTypes.h"
#include "lsp/state/DocumentStore.h"
#include "lsp/state/ModuleIndex.h"
#include "lsp/state/PositionCodec.h"
#include "lsp/state/PredefinedStubManager.h"
#include "lsp/state/SyntaxRevisionCache.h"
#include "lsp/state/WorkspaceStateStore.h"
#include "parser/AngelScriptParser.h"
#include "parser/SyntaxTree.h"
#include "utils/LspLogger.h"
#include "utils/PositionEncoding.h"
#include "utils/PreprocessorRegions.h"
#include "utils/StopFlag.h"
#include "utils/Timer.h"
#include "utils/WorkspaceIncludeGraph.h"

#include <ankerl/unordered_dense.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/io/stream.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace angel_lsp
{

/**
 * @brief High-performance LSP server implementation for the AngelScript language.
 */
class Server
{
  public:
    using OnBeforeCommitHook = std::function<void(const std::string& uri, int version, uint64_t generation)>;

    explicit Server(const angel_lsp::config::ServerConfig& config, lsp::io::Stream& stream = lsp::io::standardIO());
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void Run();
    bool ProcessIncomingMessageStep();
    void InitHandles();
    void DrainQueue();

    [[nodiscard]] WorkspaceStateStore& GetWorkspaceStore() noexcept;
    [[nodiscard]] const WorkspaceStateStore& GetWorkspaceStore() const noexcept;
    [[nodiscard]] DiagnosticsPipeline& GetDiagnosticsPipeline() noexcept;
    [[nodiscard]] const DiagnosticsPipeline& GetDiagnosticsPipeline() const noexcept;
    [[nodiscard]] LspSessionCoordinator& GetSessionCoordinator() noexcept;
    [[nodiscard]] const LspSessionCoordinator& GetSessionCoordinator() const noexcept;
    [[nodiscard]] const analysis::SymbolTable& GetSymbolTable() const;
    [[nodiscard]] DocumentStore& GetDocumentStore() noexcept;
    [[nodiscard]] const DocumentStore& GetDocumentStore() const noexcept;
    [[nodiscard]] utils::WorkspaceIncludeGraph& GetIncludeGraph() noexcept;
    [[nodiscard]] const utils::WorkspaceIncludeGraph& GetIncludeGraph() const noexcept;
    [[nodiscard]] ModuleIndex& GetModuleIndex() noexcept;
    [[nodiscard]] const ModuleIndex& GetModuleIndex() const noexcept;
    [[nodiscard]] utils::PositionEncoding GetPositionEncoding() const noexcept;
    [[nodiscard]] const angel_lsp::config::ServerConfig& GetConfig() const noexcept;
    [[nodiscard]] angel_lsp::config::ServerConfig& GetMutableConfig() noexcept;
    [[nodiscard]] analysis::SymbolTable& GetMutableSymbolTable() noexcept;
    [[nodiscard]] PredefinedStubManager& GetPredefinedManager() noexcept;
    [[nodiscard]] const analysis::CallGraphIndex& GetCallGraph() const noexcept;
    [[nodiscard]] std::optional<OpenDocument> LookupOpenDocument(const std::string& uriStr);
    void WithdrawStaleModuleDiagnostics();
    void NotifyModuleWarning(const std::string& text);
    [[nodiscard]] WorkspaceSnapshot CreateWorkspaceSnapshot() const;
    [[nodiscard]] std::vector<std::string> WorkspaceRoots() const;
    [[nodiscard]] std::shared_ptr<const std::vector<std::string>> SearchDirectories() const;
    [[nodiscard]] std::string EngineProfile() const;
    [[nodiscard]] std::vector<utils::ExcludedLineRange> ExcludedLineRanges(const std::string& text) const;
    [[nodiscard]] std::shared_ptr<const TransparentStringSet> DefinedWords() const;
    bool SetDefinedWordsFrom(const std::string& source, std::vector<std::string> words);
    [[nodiscard]] std::vector<std::string> IncludableFiles() const;
    [[nodiscard]] std::string_view ImplicitIncludeExtension() const noexcept;
    void SetOnBeforeCommitHook(OnBeforeCommitHook hook);
    void SetWorkspaceScanComplete(bool complete) noexcept;
    void SetPredefinedReady(bool ready);
    [[nodiscard]] bool IsPredefinedReady() const noexcept;
    bool WaitForPredefinedReady(std::chrono::milliseconds timeout) const;
    void ScheduleOpenDocumentsForReanalysis();
    lsp::requests::Initialize::Result HandleRequestsInitialized(lsp::requests::Initialize::Params&& params);
    static std::string DocumentKey(const std::string& uriStr);
    void ParserPredefined(const std::string& filePath, parser::AngelScriptParser& parser, bool forceReload = false);
    [[nodiscard]] analysis::formatting::BraceStyle CurrentBraceStyle() const;
    [[nodiscard]] bool CurrentSpacesInsideParentheses() const noexcept;
    [[nodiscard]] bool CurrentKeepEmptyBlocksOnSingleLine() const noexcept;
    [[nodiscard]] analysis::formatting::PointerAlignment CurrentPointerAlignment() const noexcept;
    [[nodiscard]] std::string ResolveConfiguredPath(const std::string& configured) const;
    void BindEngineConfiguration(const std::function<void(std::string_view, bool&)>& applyBool,
                                 const std::function<void(std::string_view, int&)>& applyInt);
    [[nodiscard]] std::vector<std::string> IncludeAllowedRoots() const;
    [[nodiscard]] std::shared_ptr<const std::string> FindDocumentText(const std::string& uri) const;
    static std::string UriFromPath(const std::string& path);
    template <typename ResultT, typename FeatureFn>
    ResultT ExecuteOnOpenDocument(const std::string& uriStr, bool featureEnabled, FeatureFn&& fn);
    template <typename ResultT, typename FeatureFn>
    ResultT ExecuteAtDocumentPosition(const std::string& uriStr, const lsp::Position& rawPos, bool featureEnabled,
                                      FeatureFn&& fn);
    template <typename FeatureFn>
    lsp::requests::TextDocument_Definition::Result
    DispatchDefinitionQuery(const std::string& uriStr, const lsp::Position& position, FeatureFn&& fn);
    template <typename ItemT> void EncodeItemRanges(ItemT& item) const;
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
    void EncodeRangesIn(const std::string& uri, std::vector<lsp::Range>& ranges) const;

    void LogInfo(std::string_view message) const;
    void LogWarning(std::string_view message) const;
    void LogError(std::string_view message) const;
    void LogDebug(std::string_view message) const;
    void LogTrace(std::string_view message) const;
    void LogAnalysisProfile(std::string_view prefix, const std::string& uriStr,
                            const AnalysisTimingProfile& profile) const;

    void HandleNotificationsInitialized(lsp::notifications::Initialized::Params&& params);
    lsp::requests::Shutdown::Result HandleRequestsShutdown();
    void HandleNotificationsExit();
    void HandleNotificationsWorkspace_DidChangeConfiguration(
        lsp::notifications::Workspace_DidChangeConfiguration::Params&& params);
    void HandleNotificationsWorkspace_DidChangeWatchedFiles(
        lsp::notifications::Workspace_DidChangeWatchedFiles::Params&& params);
    void HandleNotificationsWorkspace_DidChangeWorkspaceFolders(
        lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params&& params);
    void HandleNotificationsWorkspace_DidCreateFiles(lsp::notifications::Workspace_DidCreateFiles::Params&& params);
    void HandleNotificationsWorkspace_DidRenameFiles(lsp::notifications::Workspace_DidRenameFiles::Params&& params);
    void HandleNotificationsWorkspace_DidDeleteFiles(lsp::notifications::Workspace_DidDeleteFiles::Params&& params);
    void HandleNotificationsWindow_WorkDoneProgress_Cancel(
        lsp::notifications::Window_WorkDoneProgress_Cancel::Params&& params);
    void HandleNotificationsSetTrace(lsp::notifications::SetTrace::Params&& params);
    void HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params&& params);
    void HandleNotificationsTextDocument_DidChange(lsp::notifications::TextDocument_DidChange::Params&& params);
    void HandleNotificationsTextDocument_DidClose(lsp::notifications::TextDocument_DidClose::Params&& params);
    void HandleNotificationsTextDocument_WillSave(lsp::notifications::TextDocument_WillSave::Params&& params);
    void HandleNotificationsTextDocument_DidSave(lsp::notifications::TextDocument_DidSave::Params&& params);
    lsp::requests::TextDocument_WillSaveWaitUntil::Result
    HandleRequestsTextDocument_WillSaveWaitUntil(lsp::requests::TextDocument_WillSaveWaitUntil::Params&& params);
    lsp::requests::TextDocument_Diagnostic::Result
    HandleRequestsTextDocument_Diagnostic(lsp::requests::TextDocument_Diagnostic::Params&& params);
    lsp::requests::Workspace_Diagnostic::Result
    HandleRequestsWorkspace_Diagnostic(lsp::requests::Workspace_Diagnostic::Params&& params);
    lsp::requests::DocumentLink_Resolve::Result
    HandleRequestsDocumentLink_Resolve(lsp::requests::DocumentLink_Resolve::Params&& params);
    lsp::requests::InlayHint_Resolve::Result
    HandleRequestsInlayHint_Resolve(lsp::requests::InlayHint_Resolve::Params&& params);
    lsp::requests::WorkspaceSymbol_Resolve::Result
    HandleRequestsWorkspaceSymbol_Resolve(lsp::requests::WorkspaceSymbol_Resolve::Params&& params);
    lsp::requests::TextDocument_RangesFormatting::Result
    HandleRequestsTextDocument_RangesFormatting(lsp::requests::TextDocument_RangesFormatting::Params&& params);
    lsp::requests::Workspace_ExecuteCommand::Result
    HandleRequestsWorkspace_ExecuteCommand(lsp::requests::Workspace_ExecuteCommand::Params&& params);
    lsp::requests::Workspace_TextDocumentContent::Result
    HandleRequestsWorkspace_TextDocumentContent(lsp::requests::Workspace_TextDocumentContent::Params&& req);
    lsp::json::Value HandleRequestsVirtualDocumentContent(lsp::json::Value&& params);
    lsp::requests::Workspace_Symbol::Result
    HandleRequestsWorkspace_Symbol(lsp::requests::Workspace_Symbol::Params&& req);
    lsp::requests::TextDocument_LinkedEditingRange::Result
    HandleRequestsTextDocument_LinkedEditingRange(lsp::requests::TextDocument_LinkedEditingRange::Params&& req);
    lsp::requests::TextDocument_SelectionRange::Result
    HandleRequestsTextDocument_SelectionRange(lsp::requests::TextDocument_SelectionRange::Params&& req);
    lsp::requests::TextDocument_SemanticTokens_Full::Result
    HandleRequestsTextDocument_SemanticTokens_Full(lsp::requests::TextDocument_SemanticTokens_Full::Params&& req);
    lsp::requests::TextDocument_SemanticTokens_Full_Delta::Result HandleRequestsTextDocument_SemanticTokens_Full_Delta(
        lsp::requests::TextDocument_SemanticTokens_Full_Delta::Params&& req);
    lsp::requests::TextDocument_SemanticTokens_Range::Result
    HandleRequestsTextDocument_SemanticTokens_Range(lsp::requests::TextDocument_SemanticTokens_Range::Params&& req);
    lsp::requests::TextDocument_FoldingRange::Result
    HandleRequestsTextDocument_FoldingRange(lsp::requests::TextDocument_FoldingRange::Params&& req);
    lsp::requests::TextDocument_InlayHint::Result
    HandleRequestsTextDocument_InlayHint(lsp::requests::TextDocument_InlayHint::Params&& req);
    lsp::requests::TextDocument_Hover::Result
    HandleRequestsTextDocument_Hover(lsp::requests::TextDocument_Hover::Params&& req);
    lsp::requests::TextDocument_Definition::Result
    HandleRequestsTextDocument_Definition(lsp::requests::TextDocument_Definition::Params&& req);
    lsp::requests::TextDocument_Moniker::Result
    HandleRequestsTextDocument_Moniker(lsp::requests::TextDocument_Moniker::Params&& req);
    lsp::requests::TextDocument_Declaration::Result
    HandleRequestsTextDocument_Declaration(lsp::requests::TextDocument_Declaration::Params&& req);
    lsp::requests::TextDocument_Implementation::Result
    HandleRequestsTextDocument_Implementation(lsp::requests::TextDocument_Implementation::Params&& req);
    lsp::requests::TextDocument_TypeDefinition::Result
    HandleRequestsTextDocument_TypeDefinition(lsp::requests::TextDocument_TypeDefinition::Params&& req);
    lsp::requests::TextDocument_References::Result
    HandleRequestsTextDocument_References(lsp::requests::TextDocument_References::Params&& req);
    lsp::requests::TextDocument_DocumentHighlight::Result
    HandleRequestsTextDocument_DocumentHighlight(lsp::requests::TextDocument_DocumentHighlight::Params&& req);
    lsp::requests::TextDocument_Completion::Result
    HandleRequestsTextDocument_Completion(lsp::requests::TextDocument_Completion::Params&& req);
    lsp::requests::CompletionItem_Resolve::Result
    HandleRequestsCompletionItem_Resolve(lsp::requests::CompletionItem_Resolve::Params&& req);
    lsp::requests::TextDocument_SignatureHelp::Result
    HandleRequestsTextDocument_SignatureHelp(lsp::requests::TextDocument_SignatureHelp::Params&& req);
    lsp::requests::TextDocument_PrepareRename::Result
    HandleRequestsTextDocument_PrepareRename(lsp::requests::TextDocument_PrepareRename::Params&& req);
    lsp::requests::TextDocument_Rename::Result
    HandleRequestsTextDocument_Rename(lsp::requests::TextDocument_Rename::Params&& req);
    lsp::requests::TextDocument_CodeAction::Result
    HandleRequestsTextDocument_CodeAction(lsp::requests::TextDocument_CodeAction::Params&& req);
    lsp::requests::CodeAction_Resolve::Result
    HandleRequestsCodeAction_Resolve(lsp::requests::CodeAction_Resolve::Params&& req);
    lsp::requests::TextDocument_DocumentSymbol::Result
    HandleRequestsTextDocument_DocumentSymbol(lsp::requests::TextDocument_DocumentSymbol::Params&& req);
    lsp::requests::TextDocument_Formatting::Result
    HandleRequestsTextDocument_Formatting(lsp::requests::TextDocument_Formatting::Params&& req);
    lsp::requests::TextDocument_RangeFormatting::Result
    HandleRequestsTextDocument_RangeFormatting(lsp::requests::TextDocument_RangeFormatting::Params&& req);
    lsp::requests::TextDocument_OnTypeFormatting::Result
    HandleRequestsTextDocument_OnTypeFormatting(lsp::requests::TextDocument_OnTypeFormatting::Params&& req);
    lsp::requests::TextDocument_DocumentLink::Result
    HandleRequestsTextDocument_DocumentLink(lsp::requests::TextDocument_DocumentLink::Params&& req);
    lsp::requests::TextDocument_CodeLens::Result
    HandleRequestsTextDocument_CodeLens(lsp::requests::TextDocument_CodeLens::Params&& req);
    lsp::requests::CodeLens_Resolve::Result
    HandleRequestsCodeLens_Resolve(lsp::requests::CodeLens_Resolve::Params&& req);
    lsp::json::Value HandleRequestsTextDocument_BreakpointLocations(lsp::json::Value&& params);
    lsp::json::Value HandleRequestsTextDocument_EvaluatableExpression(lsp::json::Value&& params);
    lsp::json::Value HandleRequestsAngelscript_DebugCompletion(lsp::json::Value&& params);

  private:
    angel_lsp::config::ServerConfig m_config;
    std::unique_ptr<lsp::Connection> m_connection;
    std::unique_ptr<lsp::MessageHandler> m_messageHandler;
    std::unique_ptr<WorkspaceStateStore> m_workspaceStore;
    std::unique_ptr<DiagnosticsPipeline> m_diagnosticsPipeline;
    std::unique_ptr<LspSessionCoordinator> m_sessionCoordinator;
    mutable std::mutex m_lifecycleMutex;
    mutable std::mutex m_runtimeConfigMutex;
    std::function<void(const std::string& uri, int version, uint64_t generation)> m_onBeforeCommitHook;
    std::thread m_workspaceThread;
    utils::StopFlag m_workspaceStop;
    std::mutex m_messageHandlerMutex;
    std::unique_ptr<utils::LspLogger> m_logger;
    std::unique_ptr<i18n::I18n> m_i18n;
    std::unique_ptr<parser::AngelScriptParser> m_parser;
    std::unique_ptr<parser::AngelScriptParser> m_workerParser;
    analysis::SymbolTable m_symbolTable;
    std::unique_ptr<analysis::SymbolCollector> m_symbolCollector;
    analysis::ScopeIndex m_scopeIndex;
    analysis::CallGraphIndex m_callGraph;
    std::unique_ptr<analysis::LocalScopeCollector> m_localScopeCollector;
    std::unique_ptr<analysis::SemanticAnalyzer> m_semanticAnalyzer;
    PredefinedStubManager m_predefinedManager;
    ankerl::unordered_dense::map<std::string, SemanticTokensSnapshot> m_semanticTokensCache;
    mutable std::mutex m_semanticTokensMutex;
    uint64_t m_semanticTokensRevision = 0;
    mutable std::mutex m_closureMutex;
    struct ClosedDocumentSnapshot
    {
        std::shared_ptr<const std::string> text;
        uint64_t revision = 0;
        std::shared_ptr<const ClosedIndexedSyntax> indexedSyntax = {};
        std::shared_ptr<const analysis::Scope> lexicalScopes = {};
    };

    ankerl::unordered_dense::map<std::string, ClosedDocumentSnapshot> m_closureDocuments;
    uint64_t m_nextClosedRevision = 0;
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_openDocumentClosures;
    ankerl::unordered_dense::map<std::string, std::string> m_indexedUriByPath;
    ankerl::unordered_dense::map<std::string, uint64_t> m_lastSavedInterfaceHash;
    SyntaxRevisionCache m_workerSyntax;   ///< Accessed only by the scheduler callback.
    SyntaxRevisionCache m_protocolSyntax; ///< Accessed only on the LSP message loop.
    analysis::rules::MixinFileCache m_workerMixinCache;
    analysis::rules::MixinFileCache m_serverMixinCache;
    std::atomic<bool> m_clearWorkerMixinCache{false};
    mutable std::mutex m_publishedForModulesMutex;
    ankerl::unordered_dense::set<std::string> m_publishedForModules;
    std::vector<ModuleView> m_modules;
    std::vector<std::string> m_discoveredPredefined;
    std::string m_effectivePredefined;
    ankerl::unordered_dense::set<std::string> m_predefinedTransitiveIncludes;
    std::string m_workspaceProgressToken;
    unsigned m_workspaceProgressCounter = 0;

    void InitDomainServices();
    void RegisterWorkspaceHandlers();
    void RegisterTextDocumentHandlers();
    void RegisterHierarchyHandlers();
    void RegisterTokensAndFormattingHandlers();
    void BeginWorkspaceProgress(const std::string& title);
    void ReportWorkspaceProgress(const std::string& message, unsigned percentage);
    void EndWorkspaceProgress(const std::string& message);
    [[nodiscard]] int GetDocumentVersion(const std::string& uriStr) const;
    void ClearMixinCaches();
    PreparedAnalysisSource PrepareAnalysisSource(const std::string& uriStr, const std::string& text) const;
    [[nodiscard]] ModuleClaim ClaimFor(const std::string& normalizedPath) const;
    void AnalyzeConfiguredModules();
    [[nodiscard]] bool IsOpenElsewhere(const std::string& uriStr) const;
    void BuildModuleIndex();
    void AugmentSearchDirectoriesFromModules();
    void IndexConfiguredModules(parser::AngelScriptParser& parser);
    void SyncModuleIndexSymbols();
    void PurgeUnusedClosureFiles();
    [[nodiscard]] ankerl::unordered_dense::set<std::string> CollectWantedClosurePaths() const;
    [[nodiscard]] std::vector<std::string>
    CollectStaleClosureUris(const ankerl::unordered_dense::set<std::string>& wantedPaths) const;
    void CollectSharedSymbolsElsewhere(
        const ModuleView& owning,
        ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>& outShared) const;
    [[nodiscard]] std::optional<analysis::SemanticAnalysisRequest::ModuleContext>
    ModuleContextFor(const std::string& uriStr) const;
    void RestartWorkspaceScan();
    lsp::SemanticTokens ComputeAndCacheSemanticTokens(const std::string& uriStr, const std::string& text);
    void UpdateSavedDocumentCaches(const std::string& uriStr, const std::string& text,
                                   const std::vector<utils::ExcludedLineRange>& excluded);
    void HandleSavedInterfaceChange(const std::string& uriStr, bool interfaceChanged);
    void StartWorkspaceScan();
    void ReadWorkspaceFiles(const utils::StopFlag& stopToken);
    void RecoverFromScanFailure(const std::string& err);
    void LoadPredefinedIncludes(const std::string& content, const std::string& filePath, PredefinedLoadContext& ctx);
    void PreScanStubDefinedWords(const std::vector<std::string>& stubPaths);
    PredefinedIndexTiming IndexAndCommitPredefinedStub(const std::string& uri, const std::string& content,
                                                       parser::AngelScriptParser& parser);
    void ParserPredefinedInternal(const std::string& filePath, parser::AngelScriptParser& parser, bool forceReload,
                                  std::unordered_set<std::string>& visited);
    void CommitPredefinedIndex(const std::string& uri, const std::string& content, parser::TreePtr tree);
    std::vector<std::string> LoadConfiguredPredefinedFiles(parser::AngelScriptParser& parser,
                                                           const utils::StopFlag& stopToken);
    void UnloadUnselectedPredefinedStubs(const std::vector<std::string>& wantedPaths);
    bool RefreshStubDefinedWords(const std::string& uriStr, const std::string& text);
    void LoadBuiltinEngineProfiles(parser::AngelScriptParser& parser, const utils::StopFlag& stopToken);
    [[nodiscard]] std::optional<analysis::EngineProfileKind>
    ResolveTargetEngineProfile(const utils::StopFlag& stopToken);
    void UnloadStaleBuiltinEngineProfiles(const std::vector<analysis::EngineProfileKind>& wantedProfiles);
    void LoadEngineProfileStub(analysis::EngineProfileKind pKind, parser::AngelScriptParser& parser);
    void LoadWorkspaceEngineRules(const std::vector<std::string>& workspaceRoots);
    analysis::SemanticAnalysisRequest
    BuildAnalysisRequest(const std::string& uriStr, const std::string& text, const analysis::NodeIndex* nodeIndex,
                         const utils::PreprocessorScan& preprocessing,
                         const analysis::SymbolTable* customSymbolTable = nullptr) const;
    [[nodiscard]] bool IsCommitAnalysisStale(const CommitAnalysisRequest& req) const;
    bool CommitAnalysisResults(CommitAnalysisRequest req);
    std::vector<analysis::Diagnostic> ReplaceSymbolsFromTree(const std::string& uriStr, const std::string& text,
                                                             TSTree* tree, bool* outInterfaceChanged = nullptr,
                                                             const analysis::NodeIndex* index = nullptr);
    std::vector<analysis::Diagnostic> ReplaceSymbolsFromSource(const std::string& uriStr, const std::string& text,
                                                               parser::AngelScriptParser& parser);
    [[nodiscard]] bool ShouldSkipScopeAnalysis(const CollectScopesRequest& request);
    std::vector<analysis::Diagnostic> CollectScopesAndAnalyze(const CollectScopesRequest& request);
    bool ClaimPredefinedFile(const std::string& uriStr, bool forceReload = false);
    bool UnloadPredefinedUri(std::string uriStr);
    static bool PathsAreSameFile(const std::string& a, const std::string& b);
    bool PredefinedStubContributes(const std::string& uriStr) const;
    void ReportPredefinedSelection(const std::vector<std::string>& discovered, const std::string& activePath,
                                   const std::string& autoSelected, bool mergeAll);
    void PublishInactiveRegions(const std::string& uriStr, const std::vector<utils::ExcludedLineRange>& excluded);
    [[nodiscard]] bool IsPublishDiagnosticsStale(const PublishDiagnosticsRequest& request) const;
    void CacheDiagnosticsSnapshot(const PublishDiagnosticsRequest& request,
                                  const std::vector<lsp::Diagnostic>& protocolDiagnostics);
    void NotifyClientDiagnostics(lsp::notifications::TextDocument_PublishDiagnostics::Params params);
    void PublishDiagnostics(const PublishDiagnosticsRequest& request);
    void PublishDiagnostics(const std::string& uriStr, const std::vector<analysis::Diagnostic>& diagnostics,
                            int version = -1);
    void PublishDiagnostics(const std::string& uriStr, const std::string& text,
                            const std::vector<analysis::Diagnostic>& diagnostics, int version = -1,
                            std::shared_ptr<const utils::PreprocessorScan> preprocessing = {});
    std::vector<lsp::Diagnostic> ToProtocolDiagnostics(const std::string& text,
                                                       const std::vector<analysis::Diagnostic>& diagnostics,
                                                       const std::vector<utils::ExcludedLineRange>& excluded) const;
    [[nodiscard]] std::optional<lsp::requests::TextDocument_Diagnostic::Result>
    TryGetCachedDiagnosticReport(const std::string& uriStr, size_t currentHash, int currentVersion,
                                 uint64_t closedRevision, const std::optional<std::string>& previousResultId);
    [[nodiscard]] std::optional<uint64_t> ClosedDiagnosticRevision(const std::string& uriStr,
                                                                   const std::string* source) const;
    std::string GenerateVirtualMixinDocument(std::string_view uri);
    void DidSavePredefinedFile(const std::string& uriStr, const std::string& text);
    void ReanalyzeDependentsOnSave(const std::string& uriStr);
    void ReanalyzeClosedModuleDependents(const std::string& savedUriStr, const ModuleClaim& savedClaim,
                                         const ankerl::unordered_dense::set<std::string>& closureSet);
    std::string ReadClosedDocumentContent(const std::string& uriStr);
    void DidOpenPredefinedFile(const DidOpenPredefinedRequest& req);
    bool RestoreClosedModuleFile(const std::string& uriStr, const std::string& path);
    lsp::requests::Workspace_ExecuteCommand::Result
    ExecuteFormatPredefinedStub(const std::optional<lsp::Array<lsp::LSPAny>>& args);
    lsp::requests::Workspace_ExecuteCommand::Result ExecuteListPredefinedStubs() const;
    bool CollectWorkspaceFiles(const std::vector<std::string>& roots, const utils::StopFlag& stopToken,
                               WorkspaceFilesWalkResult& outFiles);
    bool LoadAndProcessPredefinedStubs(const std::vector<std::string>& discoveredStubPaths,
                                       const utils::StopFlag& stopToken, parser::AngelScriptParser& parser);
    void ProcessDiscoveredPredefinedStubs(const std::vector<std::string>& discoveredStubPaths,
                                          const std::vector<std::string>& configuredPaths,
                                          const utils::StopFlag& stopToken, parser::AngelScriptParser& parser);
    void BuildIncludeGraphAndModules(const std::vector<std::string>& allScriptFiles,
                                     const std::vector<std::string>& roots, const utils::StopFlag& stopToken,
                                     parser::AngelScriptParser& parser);
    bool HandleWatchedFileDeleted(const std::string& path, const std::string& uriStr, bool isPredefined,
                                  bool& stubSelectionInvalidated);
    bool HandleWatchedFileChanged(const std::string& path, bool isPredefined, parser::AngelScriptParser& parser);
    void ReplaceDeletedPredefinedStub(parser::AngelScriptParser& parser);
    void RefreshOpenDocumentIncludes();
    static std::string CanonicalPathFromUri(const std::string& uriStr);
    std::shared_ptr<const document::Document> ResolveVirtualDocument(const std::string& key, const std::string& uriStr);
    std::optional<OpenDocument> LookupPreindexedFallback(const std::string& key);
    [[nodiscard]] std::optional<std::string> OccurrenceModuleFor(const std::string& uri) const;
    features::DefinitionRequest MakeDefinitionRequest(const OpenDocument& doc, const lsp::Position& position);
    std::optional<RenameContext> SetupRenameContext(const std::string& uriStr, const lsp::Position& rawPos);
    void InvalidateCreatedClosureFiles(const std::vector<std::string>& createdPaths);
    void ReanalyseOpenDocuments();
    std::vector<std::string> ComputeModuleClosure(const std::string& openPath) const;
    ankerl::unordered_dense::set<std::string> ComputeModuleClosureUris(const std::string& uriStr) const;
    size_t IndexModuleClosure(const std::string& openUriStr, SyntaxRevisionCache* syntaxCache = nullptr);
    void ReleaseModuleClosure(const std::string& openUriStr);
    void IndexClosureFile(const std::string& path, parser::AngelScriptParser& parser,
                          SyntaxRevisionCache* syntaxCache = nullptr);
    void ReindexTrackedClosureFile(const std::string& path, angel_lsp::parser::AngelScriptParser& parser);
    void WithdrawClosedSnapshot(const std::string& uriStr);
    void PurgeClosureFile(const std::string& uriStr);
    void AppendIncludeDiagnostics(const std::string& uriStr, const std::string& text,
                                  const std::vector<utils::ExcludedLineRange>& excluded,
                                  std::vector<analysis::Diagnostic>& diagnostics);
    [[nodiscard]] std::optional<ScheduleRequest> MakeScheduleRequest(ScheduleAnalysisRequest req) const;
    [[nodiscard]] bool IsClosedRevisionCurrentLocked(const std::string& uri, uint64_t revision) const;
    void ScheduleAnalysis(ScheduleAnalysisRequest req);
    void ScheduleAnalysis(const std::string& uriStr, const std::string& text, bool force = false);
    void ScheduleAnalysisImmediate(ScheduleAnalysisRequest req);
    [[nodiscard]] bool IsAnalyzeDocumentStale(const AnalyzeDocumentRequest& req) const;
    void StorePredefinedSnapshot(const AnalyzeDocumentRequest& req, const std::string& analysisText,
                                 const parser::TreePtr& tree, bool contributes);
    bool TryZeroCopyPredefinedAnalysis(const AnalyzeDocumentRequest& req, const std::string& analysisText,
                                       const utils::HighResTimer& totalTimer);
    void AnalyzePredefinedDocument(AnalyzeDocumentRequest req, const utils::HighResTimer& totalTimer);
    ParseAndCollectResult ParseAndCollectSymbols(AnalyzeDocumentRequest& req, analysis::SymbolTable& staging,
                                                 SyntaxRevisionCache* syntaxCache = nullptr);
    /// Resolves an exact revision on the message loop, copying the document tree when necessary.
    [[nodiscard]] SyntaxRevision ResolveProtocolSyntax(std::string_view uri, std::string_view source,
                                                       const document::Document* open);
    std::optional<ParseAndCollectResult> PrepareAnalysisDocument(AnalyzeDocumentRequest& req,
                                                                 analysis::SymbolTable& staging);
    std::vector<analysis::Diagnostic> RunDocumentSemantics(analysis::SemanticAnalysisRequest baseReq,
                                                           double& outCheckMs, bool fromScheduler);
    void AnalyzeNormalDocument(AnalyzeDocumentRequest req, const utils::HighResTimer& totalTimer);
    const analysis::SymbolTable* PrepareAnalysisSnapshot(const std::string& uriStr,
                                                         const analysis::SymbolTable* staging,
                                                         std::unique_ptr<analysis::SymbolTable>& outSnapshot) const;
    bool IsAnalysisRequestStale(const AnalyzeDocumentRequest& req) const;
    void CommitSyntaxOnlyResults(const AnalyzeDocumentRequest& req, std::vector<analysis::Diagnostic> diags);
    [[nodiscard]] ClosedAnalysisInputs FindClosedAnalysisInputs(const AnalyzeDocumentRequest& req) const;
    std::pair<std::shared_ptr<analysis::Scope>, std::vector<analysis::CallSite>>
    CollectScopesAndCalls(const AnalyzeDocumentRequest& req, const ParseAndCollectResult& parsed);
    void AnalyzeDocument(AnalyzeDocumentRequest req);
};

inline WorkspaceStateStore& Server::GetWorkspaceStore() noexcept
{
    return *m_workspaceStore;
}
inline const WorkspaceStateStore& Server::GetWorkspaceStore() const noexcept
{
    return *m_workspaceStore;
}
inline DiagnosticsPipeline& Server::GetDiagnosticsPipeline() noexcept
{
    return *m_diagnosticsPipeline;
}
inline const DiagnosticsPipeline& Server::GetDiagnosticsPipeline() const noexcept
{
    return *m_diagnosticsPipeline;
}
inline LspSessionCoordinator& Server::GetSessionCoordinator() noexcept
{
    return *m_sessionCoordinator;
}
inline const LspSessionCoordinator& Server::GetSessionCoordinator() const noexcept
{
    return *m_sessionCoordinator;
}
inline const analysis::SymbolTable& Server::GetSymbolTable() const
{
    return m_symbolTable;
}
inline analysis::SymbolTable& Server::GetMutableSymbolTable() noexcept
{
    return m_symbolTable;
}
inline PredefinedStubManager& Server::GetPredefinedManager() noexcept
{
    return m_predefinedManager;
}
inline DocumentStore& Server::GetDocumentStore() noexcept
{
    return m_workspaceStore->GetDocumentStore();
}
inline const DocumentStore& Server::GetDocumentStore() const noexcept
{
    return m_workspaceStore->GetDocumentStore();
}
inline utils::WorkspaceIncludeGraph& Server::GetIncludeGraph() noexcept
{
    return m_workspaceStore->GetIncludeGraph();
}
inline const utils::WorkspaceIncludeGraph& Server::GetIncludeGraph() const noexcept
{
    return m_workspaceStore->GetIncludeGraph();
}
inline ModuleIndex& Server::GetModuleIndex() noexcept
{
    return m_workspaceStore->GetModuleIndex();
}
inline const ModuleIndex& Server::GetModuleIndex() const noexcept
{
    return m_workspaceStore->GetModuleIndex();
}
inline utils::PositionEncoding Server::GetPositionEncoding() const noexcept
{
    return m_sessionCoordinator->GetPositionEncoding();
}
inline std::vector<std::string> Server::IncludableFiles() const
{
    return m_workspaceStore->GetIncludeGraph().AllFiles();
}
inline void Server::SetOnBeforeCommitHook(OnBeforeCommitHook hook)
{
    m_onBeforeCommitHook = std::move(hook);
}
inline int Server::GetDocumentVersion(const std::string& uriStr) const
{
    return GetDocumentStore().GetVersion(uriStr);
}
inline void Server::ClearMixinCaches()
{
    m_serverMixinCache.clear();
    m_clearWorkerMixinCache.store(true, std::memory_order_release);
}
inline void Server::LogInfo(std::string_view message) const
{
    if (m_logger)
        m_logger->LogInfo(message);
}
inline void Server::LogWarning(std::string_view message) const
{
    if (m_logger)
        m_logger->LogWarning(message);
}
inline void Server::LogError(std::string_view message) const
{
    if (m_logger)
        m_logger->LogError(message);
}
inline void Server::LogDebug(std::string_view message) const
{
    if (m_logger)
        m_logger->LogDebug(message);
}
inline void Server::LogTrace(std::string_view message) const
{
    if (m_logger)
        m_logger->LogTrace(message);
}
inline const angel_lsp::config::ServerConfig& Server::GetConfig() const noexcept
{
    return m_config;
}
inline angel_lsp::config::ServerConfig& Server::GetMutableConfig() noexcept
{
    return m_config;
}
inline const analysis::CallGraphIndex& Server::GetCallGraph() const noexcept
{
    return m_callGraph;
}

template <typename ResultT, typename FeatureFn>
ResultT Server::ExecuteOnOpenDocument(const std::string& uriStr, bool featureEnabled, FeatureFn&& fn)
{
    if (!featureEnabled)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(uriStr);
    if (!doc)
    {
        return lsp::Null{};
    }
    return fn(*doc);
}

template <typename ResultT, typename FeatureFn>
ResultT Server::ExecuteAtDocumentPosition(const std::string& uriStr, const lsp::Position& rawPos, bool featureEnabled,
                                          FeatureFn&& fn)
{
    if (!featureEnabled)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(uriStr);
    if (!doc)
    {
        return lsp::Null{};
    }
    const lsp::Position decodedPos = codec::Decode(*doc->text, GetPositionEncoding(), rawPos);
    return fn(*doc, decodedPos);
}

template <typename ItemT> void Server::EncodeItemRanges(ItemT& item) const
{
    if (GetPositionEncoding() == utils::PositionEncoding::Utf8)
    {
        return;
    }
    if (auto text = FindDocumentText(item.uri.toString()))
    {
        EncodeIn(*text, item.range);
        EncodeIn(*text, item.selectionRange);
    }
}

} // namespace angel_lsp