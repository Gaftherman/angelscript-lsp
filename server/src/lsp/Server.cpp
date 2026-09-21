#include "Server.h"
#include "analysis/EngineProfiles.h"
#include "features/call_hierarchy/CallHierarchyHandler.h"
#include "features/code_action/CodeActionHandler.h"
#include "features/code_lens/CodeLensHandler.h"
#include "features/completion/CompletionHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/document_highlight/DocumentHighlightHandler.h"
#include "features/document_link/DocumentLinkHandler.h"
#include "features/document_symbol/DocumentSymbolHandler.h"
#include "features/folding_range/FoldingRangeHandler.h"
#include "features/formatting/FormattingHandler.h"
#include "features/formatting/PredefinedStubFormatter.h"
#include "features/hover/HoverHandler.h"
#include "features/implementation/ImplementationHandler.h"
#include "features/inlay_hint/InlayHintHandler.h"
#include "features/linked_editing/LinkedEditingRangeHandler.h"
#include "features/references/ReferencesHandler.h"
#include "features/rename/RenameHandler.h"
#include "features/selection_range/SelectionRangeHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "features/signature_help/SignatureHelpHandler.h"
#include "features/type_hierarchy/TypeHierarchyHandler.h"
#include "features/workspace_symbol/WorkspaceSymbolHandler.h"
#include "lsp/PositionCodec.h"
#include "utils/Constants.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include "utils/WorkspaceScan.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <variant>

namespace angel_lsp
{
namespace
{
/**
 * @brief How many message errors in a row are tolerated before the session is closed.
 *
 * A malformed frame is recoverable and must not end the session, but a stream that fails
 * identically forever is not: without a ceiling, Run() would spin at full tilt on a frame
 * it can neither consume nor skip. Reset to zero by any message that dispatches cleanly.
 */
constexpr unsigned k_maxConsecutiveMessageErrors = 64;

/**
 * @brief Reads the configured brace style name.
 *
 * Only "kr" - and its spellings - selects K&R. Anything else, an empty string and a typo
 * alike, is Allman, which is the default and the style every existing test asserts. A
 * setting nobody can misspell into a surprise is worth more here than a diagnostic about
 * a formatter option.
 */
bool BraceStyleIsKR(std::string_view name)
{
    std::string lowered;
    lowered.reserve(name.size());
    for (char c : name)
    {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered == "kr" || lowered == "k&r" || lowered == "kandr" || lowered == "onetbs";
}

/**
 * @brief Maps this analyzer's severity onto the protocol's.
 *
 * Not a cast, though it was one until an end-to-end test looked at what actually went over
 * the wire. The two enumerations disagree about where they start: analysis::
 * DiagnosticSeverity numbers Error as 1 to match the LSP wire values directly, while the
 * generated lsp::DiagnosticSeverity is an ordinary 0-based enum whose serializer maps its
 * index onto {1,2,3,4}. Casting between them shifted every diagnostic one step - every
 * error the server had ever published arrived in the editor as a warning - and pushed Hint
 * off the end of the table, where it serialized as 0 and meant nothing at all.
 */
lsp::DiagnosticSeverity ToProtocolSeverity(analysis::DiagnosticSeverity severity)
{
    switch (severity)
    {
    case analysis::DiagnosticSeverity::Error:
        return lsp::DiagnosticSeverity::Error;
    case analysis::DiagnosticSeverity::Warning:
        return lsp::DiagnosticSeverity::Warning;
    case analysis::DiagnosticSeverity::Information:
        return lsp::DiagnosticSeverity::Information;
    case analysis::DiagnosticSeverity::Hint:
        return lsp::DiagnosticSeverity::Hint;
    }
    return lsp::DiagnosticSeverity::Error;
}

/**
 * @brief Searches for a boolean setting under direct, dot-prefixed, or fully-qualified engine keys.
 * @param[in] initSection Configuration object section.
 * @param[in] engineObj Nested engine configuration object if present.
 * @param[in] name Setting property name.
 * @return Value of the boolean setting if present.
 */
std::optional<bool> FindSectionBool(const lsp::LSPObject* initSection, const lsp::LSPObject* engineObj,
                                    std::string_view name)
{
    if (engineObj)
    {
        if (const auto* val = engineObj->find(std::string(name)); val && val->isBoolean())
        {
            return val->boolean();
        }
    }
    const std::string dotKey = "engine." + std::string(name);
    if (const auto* val = initSection->find(dotKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    const std::string fullKey = "angelscript.engine." + std::string(name);
    if (const auto* val = initSection->find(fullKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    return std::nullopt;
}

/**
 * @brief Searches for an integer setting under direct, dot-prefixed, or fully-qualified engine keys.
 * @param[in] initSection Configuration object section.
 * @param[in] engineObj Nested engine configuration object if present.
 * @param[in] name Setting property name.
 * @return Value of the integer setting if present.
 */
std::optional<int> FindSectionInt(const lsp::LSPObject* initSection, const lsp::LSPObject* engineObj,
                                  std::string_view name)
{
    if (engineObj)
    {
        if (const auto* val = engineObj->find(std::string(name)); val && val->isNumber())
        {
            return static_cast<int>(val->number());
        }
    }
    const std::string dotKey = "engine." + std::string(name);
    if (const auto* val = initSection->find(dotKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    const std::string fullKey = "angelscript.engine." + std::string(name);
    if (const auto* val = initSection->find(fullKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    return std::nullopt;
}
} // namespace

Server::Server(const angel_lsp::config::ServerConfig& config, lsp::io::Stream& stream)
{
    m_config = config;

    m_connection = std::make_unique<lsp::Connection>(stream);
    m_messageHandler = std::make_unique<lsp::MessageHandler>(*m_connection);

    m_running = true;

    m_logger = std::make_unique<angel_lsp::utils::LspLogger>(m_messageHandler.get());
    if (m_logger)
    {
        m_logger->SetLevel(angel_lsp::utils::ParseLogLevel(m_config.info.logLevel, angel_lsp::utils::LogLevel::Info));
    }

    m_parser = std::make_unique<angel_lsp::parser::AngelScriptParser>(m_logger.get());
    m_workerParser = std::make_unique<angel_lsp::parser::AngelScriptParser>(m_logger.get());

    m_symbolCollector = std::make_unique<angel_lsp::analysis::SymbolCollector>(m_logger.get());

    m_localScopeCollector = std::make_unique<angel_lsp::analysis::LocalScopeCollector>(m_logger.get());

    m_semanticAnalyzer = std::make_unique<angel_lsp::analysis::SemanticAnalyzer>(m_logger.get());

    m_i18n = std::make_unique<angel_lsp::i18n::I18n>(m_config.info.locale.empty() ? "en" : m_config.info.locale);

    // Seeded from the startup config and mutable from here on. Everything that reads these
    // three goes through the accessors below; m_config's own copies are not read again.
    m_searchDirectories = std::make_shared<const std::vector<std::string>>(m_config.searchDirectories);
    m_engineProfile = m_config.engineProfile;

    // Seeded empty so DefinedWords() never hands back a null snapshot, then filled from the
    // flag and the client setting. Stubs add theirs as they load.
    m_definedWords = std::make_shared<const ankerl::unordered_dense::set<std::string>>();
    SetDefinedWordsFrom(std::string(), m_config.definedWords);
    m_formatBraceStyleKR.store(BraceStyleIsKR(m_config.format.braceStyle), std::memory_order_relaxed);

    BuildDiagnosticSeverityOverrides();

    InitHandles();

    m_analysisScheduler = std::make_unique<angel_lsp::AnalysisScheduler>(
        [this](angel_lsp::AnalyzeRequest req)
        {
            AnalyzeDocument({.uriStr = std::move(req.uriStr),
                             .text = std::move(req.text),
                             .parser = *m_workerParser,
                             .treeCopy = std::move(req.tree),
                             .version = req.version,
                             .generation = req.generation,
                             .configRevision = req.configRevision});
        });
}

Server::~Server()
{
    m_running = false;

    if (m_analysisScheduler)
    {
        m_analysisScheduler->Stop();
    }

    m_workspaceStop.Request();
    if (m_workspaceThread.joinable())
        m_workspaceThread.join();

    m_documentStore.Clear();
    m_predefinedManager.Clear();
}

std::vector<std::string> Server::WorkspaceRoots() const
{
    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
    return m_workspacesRoot;
}

std::shared_ptr<const std::vector<std::string>> Server::SearchDirectories() const
{
    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
    return m_searchDirectories;
}

std::string Server::EngineProfile() const
{
    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
    return m_engineProfile;
}

std::shared_ptr<const ankerl::unordered_dense::set<std::string>> Server::DefinedWords() const
{
    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
    return m_definedWords;
}

bool Server::SetDefinedWordsFrom(const std::string& source, std::vector<std::string> words)
{
    auto merged = std::make_shared<ankerl::unordered_dense::set<std::string>>();

    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);

    if (words.empty())
        m_definedWordsBySource.erase(source);
    else
        m_definedWordsBySource[source] = std::move(words);

    for (const auto& [_, contributed] : m_definedWordsBySource)
    {
        for (const auto& word : contributed)
            merged->insert(word);
    }

    if (m_definedWords && *m_definedWords == *merged)
        return false;

    m_definedWords = std::move(merged);
    return true;
}

void Server::Run()
{
    // A malformed message must not end the session. The framework rethrows json::ParseError
    // and jsonrpc::ProtocolError out of readMessage after having already written the JSON-RPC
    // error response - its own source calls that a FIXME - and neither derives from
    // ConnectionError, so before this loop caught them they escaped main() and hit
    // std::terminate. One stray byte from the client killed the server, and ~Server never ran,
    // which meant the analysis and workspace threads were torn down mid-flight.
    //
    // Recovery is safe because the offending frame was fully consumed before the throw: the
    // stream is still aligned on a message boundary and the next read starts on a fresh header.
    unsigned consecutiveErrors = 0;

    while (m_running)
    {
        // A closed transport is how this process normally ends: the editor exits, stdin hits
        // end of file, and the framework reports it by throwing. Letting that escape main()
        // would turn an ordinary shutdown into a crash, and the destructors that join the
        // background threads would never run.
        try
        {
            m_messageHandler->processIncomingMessages();
            consecutiveErrors = 0;
            continue;
        }
        catch (const lsp::ConnectionError& e)
        {
            LogInfo(fmt::format("Connection closed: {}", e.what()));
            m_running = false;
            continue;
        }
        catch (const lsp::io::Error& e)
        {
            LogInfo(fmt::format("Transport closed: {}", e.what()));
            m_running = false;
            continue;
        }
        catch (const lsp::json::ParseError& e)
        {
            LogError(fmt::format("Malformed JSON-RPC message discarded: {}", e.what()));
        }
        catch (const lsp::jsonrpc::ProtocolError& e)
        {
            LogError(fmt::format("Protocol error, message discarded: {}", e.what()));
        }
        catch (const std::exception& e)
        {
            // A bug in one handler is not a reason to drop the session. The transport wraps
            // everything it does not recognise into ConnectionError, so anything arriving here
            // came from message dispatch, not from the stream.
            LogError(fmt::format("Unhandled exception handling message: {}", e.what()));
        }

        // Guard against a stream that fails the same way forever - recovering from a frame we
        // cannot consume would spin this loop at full tilt with no way out.
        if (++consecutiveErrors >= k_maxConsecutiveMessageErrors)
        {
            LogError(
                fmt::format("Giving up after {} consecutive message errors; closing the session.", consecutiveErrors));
            m_running = false;
        }
    }
}

void Server::ExtractInitialWorkspaceRoots(const lsp::requests::Initialize::Params& params)
{
    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
    if (params.workspaceFolders.has_value() && !params.workspaceFolders.value().isNull())
    {
        for (const auto& workspace : params.workspaceFolders.value().value())
        {
            const std::string fsPath = workspace.uri.fsPath();
            if (!fsPath.empty())
            {
                m_workspacesRoot.push_back(angel_lsp::utils::IncludeResolver::NormalizePath(fsPath));
            }
        }
    }

    if (m_workspacesRoot.empty())
    {
        std::string root;
        if (!params.rootUri.isNull())
        {
            root = params.rootUri.value().fsPath();
            if (root.empty())
            {
                root = angel_lsp::utils::UriToPath(params.rootUri.value().toString());
            }
        }
        if (root.empty() && params.rootPath.has_value() && !params.rootPath.value().isNull())
        {
            root = angel_lsp::utils::UriToPath(params.rootPath.value().value());
        }
        if (!root.empty())
        {
            m_workspacesRoot.push_back(angel_lsp::utils::IncludeResolver::NormalizePath(root));
        }
    }
}

void Server::ApplyEngineInitializationOptions(const std::optional<lsp::LSPAny>& initOpts)
{
    if (!initOpts.has_value() || !initOpts->isObject())
    {
        return;
    }

    const lsp::LSPObject* initSection = &initOpts->object();
    if (const auto* nested = initSection->find("angelscript"); nested && nested->isObject())
    {
        initSection = &nested->object();
    }
    const lsp::LSPObject* engineObj = nullptr;
    if (const auto* e = initSection->find("engine"); e && e->isObject())
    {
        engineObj = &e->object();
    }

    if (auto v = FindSectionBool(initSection, engineObj, "foreachSupport"))
        m_config.engine.foreachSupport = *v;
    if (auto v = FindSectionBool(initSection, engineObj, "requireEnumScope"))
        m_config.engine.requireEnumScope = *v;
    if (auto v = FindSectionBool(initSection, engineObj, "alwaysImplDefaultConstruct"))
        m_config.engine.alwaysImplDefaultConstruct = *v;
    if (auto v = FindSectionBool(initSection, engineObj, "allowUnicodeIdentifiers"))
        m_config.engine.allowUnicodeIdentifiers = *v;
    if (auto v = FindSectionBool(initSection, engineObj, "ignoreDuplicateSharedIntf"))
        m_config.engine.ignoreDuplicateSharedIntf = *v;
    if (auto v = FindSectionInt(initSection, engineObj, "compilerWarnings"))
        m_config.engine.compilerWarnings = *v;
}

void Server::NegotiateClientCapabilities(const lsp::ClientCapabilities& capabilities)
{
    m_positionEncoding = angel_lsp::utils::PositionEncoding::Utf16;
    if (capabilities.general.has_value() && capabilities.general->positionEncodings.has_value())
    {
        for (const auto& offered : capabilities.general->positionEncodings.value())
        {
            if (offered == lsp::PositionEncodingKind::UTF8)
            {
                m_positionEncoding = angel_lsp::utils::PositionEncoding::Utf8;
                break;
            }
        }
    }

    if (capabilities.window.has_value() && capabilities.window->workDoneProgress.has_value())
    {
        m_workDoneProgressSupport = capabilities.window->workDoneProgress.value();
    }

    m_clientPullsDiagnostics =
        capabilities.textDocument.has_value() && capabilities.textDocument->diagnostic.has_value();

    m_clientSupportsDiagnosticRefresh = capabilities.workspace.has_value() &&
                                        capabilities.workspace->diagnostics.has_value() &&
                                        capabilities.workspace->diagnostics->refreshSupport.has_value() &&
                                        capabilities.workspace->diagnostics->refreshSupport.value();

    if (capabilities.textDocument.has_value() && capabilities.textDocument->completion.has_value() &&
        capabilities.textDocument->completion->completionItem.has_value() &&
        capabilities.textDocument->completion->completionItem->snippetSupport.has_value())
    {
        m_snippetSupport = capabilities.textDocument->completion->completionItem->snippetSupport.value();
    }
}

void Server::ConfigureNavigationAndEditingCapabilities(lsp::ServerCapabilities& caps) const
{
    if (m_config.features.enableHover)
    {
        caps.hoverProvider = true;
    }

    if (m_config.features.enableDefinition)
    {
        caps.definitionProvider = true;
        caps.typeDefinitionProvider = true;
        caps.declarationProvider = true;
        caps.monikerProvider = true;
    }

    if (m_config.features.enableImplementation)
    {
        caps.implementationProvider = true;
    }

    if (m_config.features.enableSelectionRange)
    {
        caps.selectionRangeProvider = true;
    }

    if (m_config.features.enableCallHierarchy)
    {
        caps.callHierarchyProvider = true;
    }

    if (m_config.features.enableTypeHierarchy)
    {
        caps.typeHierarchyProvider = true;
    }

    if (m_config.features.enableLinkedEditing)
    {
        caps.linkedEditingRangeProvider = true;
    }

    if (m_config.features.enableCodeLens)
    {
        lsp::CodeLensOptions codeLensOpts;
        codeLensOpts.resolveProvider = true;
        caps.codeLensProvider = codeLensOpts;
    }
}

void Server::ConfigureEditingAndSymbolCapabilities(lsp::ServerCapabilities& caps) const
{
    if (m_config.features.enableCompletion)
    {
        lsp::CompletionOptions completionOpts;
        completionOpts.triggerCharacters = lsp::Array<lsp::String>{".", ":"};
        completionOpts.resolveProvider = true;
        caps.completionProvider = completionOpts;
    }

    if (m_config.features.enableSemanticTokens)
    {
        lsp::SemanticTokensOptions semOpts;
        semOpts.legend = features::GetSemanticTokensLegend();
        lsp::SemanticTokensFullDelta fullDelta;
        fullDelta.delta = true;
        semOpts.full = fullDelta;
        semOpts.range = true;
        caps.semanticTokensProvider = semOpts;
    }

    if (m_config.features.enableSignatureHelp)
    {
        lsp::SignatureHelpOptions sigOpts;
        sigOpts.triggerCharacters = lsp::Array<lsp::String>{"(", ","};
        caps.signatureHelpProvider = sigOpts;
    }

    if (m_config.features.enableDocumentSymbols)
    {
        caps.documentSymbolProvider = true;
    }

    if (m_config.features.enableWorkspaceSymbols)
    {
        lsp::WorkspaceSymbolOptions wsOpts;
        wsOpts.resolveProvider = true;
        caps.workspaceSymbolProvider = wsOpts;
    }

    if (m_config.features.enableReferences)
    {
        caps.referencesProvider = true;
    }

    if (m_config.features.enableRename)
    {
        lsp::RenameOptions renameOpts;
        renameOpts.prepareProvider = true;
        caps.renameProvider = renameOpts;
    }

    if (m_config.features.enableDocumentHighlight)
    {
        caps.documentHighlightProvider = true;
    }

    if (m_config.features.enableFoldingRange)
    {
        caps.foldingRangeProvider = true;
    }

    if (m_config.features.enableInlayHints)
    {
        lsp::InlayHintOptions inlayOpts;
        inlayOpts.resolveProvider = true;
        caps.inlayHintProvider = inlayOpts;
    }
}

void Server::ConfigureFormattingAndDiagnosticCapabilities(lsp::ServerCapabilities& caps) const
{
    if (m_config.features.enableCodeAction)
    {
        lsp::CodeActionOptions codeActionOpts;
        codeActionOpts.resolveProvider = true;
        codeActionOpts.codeActionKinds =
            lsp::Array<lsp::CodeActionKindEnum>{lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix),
                                                lsp::CodeActionKindEnum(lsp::CodeActionKind::Refactor),
                                                lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract),
                                                lsp::CodeActionKindEnum(lsp::CodeActionKind::SourceOrganizeImports)};
        caps.codeActionProvider = codeActionOpts;
    }

    if (m_config.features.enableFormatting)
    {
        caps.documentFormattingProvider = true;
        lsp::DocumentRangeFormattingOptions rangeOpts;
        rangeOpts.rangesSupport = true;
        caps.documentRangeFormattingProvider = rangeOpts;
    }

    if (m_config.features.enableOnTypeFormatting)
    {
        lsp::DocumentOnTypeFormattingOptions onTypeOpts;
        onTypeOpts.firstTriggerCharacter = ";";
        onTypeOpts.moreTriggerCharacter = lsp::Array<lsp::String>{"}", "\n"};
        caps.documentOnTypeFormattingProvider = onTypeOpts;
    }

    if (m_config.features.enableDocumentLink)
    {
        lsp::DocumentLinkOptions linkOpts;
        linkOpts.resolveProvider = true;
        caps.documentLinkProvider = linkOpts;
    }

    if (m_config.features.enablePullDiagnostics)
    {
        lsp::DiagnosticOptions diagnosticOpts;
        diagnosticOpts.identifier = std::string("angelscript");
        diagnosticOpts.interFileDependencies = true;
        diagnosticOpts.workspaceDiagnostics = true;
        caps.diagnosticProvider = diagnosticOpts;
    }
}

void Server::ConfigureWorkspaceCapabilities(lsp::ServerCapabilities& caps) const
{
    lsp::WorkspaceFoldersServerCapabilities folderCaps;
    folderCaps.supported = true;
    folderCaps.changeNotifications = true;

    lsp::WorkspaceOptions workspaceOpts;
    workspaceOpts.workspaceFolders = folderCaps;

    lsp::FileOperationPattern scriptPattern;
    scriptPattern.glob = fmt::format("**/*{}", m_config.info.fileExtension);

    lsp::FileOperationFilter scriptFilter;
    scriptFilter.pattern = scriptPattern;
    scriptFilter.scheme = std::string("file");

    lsp::FileOperationRegistrationOptions registration;
    registration.filters = lsp::Array<lsp::FileOperationFilter>{scriptFilter};

    lsp::FileOperationOptions fileOps;
    fileOps.didCreate = registration;
    fileOps.didRename = registration;
    fileOps.didDelete = registration;
    workspaceOpts.fileOperations = fileOps;

    lsp::TextDocumentContentOptions contentOpts;
    lsp::Array<lsp::String> schemes{"angelscript-predefined"};
    if (m_config.features.enableVirtualMixinDocuments)
    {
        schemes.push_back("angelscript-virtual");
    }
    contentOpts.schemes = schemes;
    workspaceOpts.textDocumentContent = contentOpts;

    caps.workspace = workspaceOpts;

    lsp::ExecuteCommandOptions cmdOpts;
    cmdOpts.commands = lsp::Array<lsp::String>{"angelscript.rescanWorkspace", "angelscript.listPredefinedStubs"};
    caps.executeCommandProvider = cmdOpts;
}

lsp::ServerCapabilities Server::BuildServerCapabilities() const
{
    lsp::ServerCapabilities caps;
    caps.positionEncoding = (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
                                ? lsp::PositionEncodingKind::UTF8
                                : lsp::PositionEncodingKind::UTF16;

    lsp::TextDocumentSyncOptions sync;
    sync.openClose = true;
    sync.change = lsp::TextDocumentSyncKind::Incremental;
    sync.willSave = true;
    sync.willSaveWaitUntil = true;

    lsp::SaveOptions saveOptions;
    saveOptions.includeText = true;
    sync.save = saveOptions;
    caps.textDocumentSync = sync;

    ConfigureNavigationAndEditingCapabilities(caps);
    ConfigureEditingAndSymbolCapabilities(caps);
    ConfigureFormattingAndDiagnosticCapabilities(caps);
    ConfigureWorkspaceCapabilities(caps);

    return caps;
}

lsp::requests::Initialize::Result Server::HandleRequestsInitialized(lsp::requests::Initialize::Params&& params)
{
    ExtractInitialWorkspaceRoots(params);
    ApplyEngineInitializationOptions(params.initializationOptions);

    m_i18n = std::make_unique<angel_lsp::i18n::I18n>(
        params.locale.value_or(m_config.info.locale.empty() ? "en" : m_config.info.locale));

    NegotiateClientCapabilities(params.capabilities);

    const bool useUtf8 = (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8);
    LogInfo(fmt::format("Negotiated position encoding: {}", useUtf8 ? "utf-8" : "utf-16"));
    m_symbolTable.SetVirtualMixinDocumentsEnabled(m_config.features.enableVirtualMixinDocuments);

    lsp::requests::Initialize::Result result;
    lsp::ServerInfo info;
    info.name = m_config.info.name;
    info.version = m_config.info.version;
    result.serverInfo = info;
    result.capabilities = BuildServerCapabilities();

    return result;
}

void Server::HandleNotificationsInitialized([[maybe_unused]] lsp::notifications::Initialized::Params&& params)
{
    // Started unconditionally: even with the predefined-stub loader disabled, the workspace
    // thread still has to build the #include graph that module-closure indexing depends on.
    StartWorkspaceScan();
}

lsp::requests::Shutdown::Result Server::HandleRequestsShutdown()
{
    m_running = false;
    return lsp::requests::Shutdown::Result{};
}

void Server::HandleNotificationsExit()
{
    m_running = false;
}

lsp::SemanticTokens Server::ComputeAndCacheSemanticTokens(const std::string& uriStr, const std::string& text)
{
    TSTree* tree = m_documentStore.GetTree(uriStr);
    int currentVersion = m_documentStore.GetVersion(uriStr);

    analysis::NodeIndex localNodeIndex;
    const analysis::NodeIndex* nodeIndexPtr = nullptr;
    if (tree)
    {
        localNodeIndex.Build(ts_tree_root_node(tree));
        nodeIndexPtr = &localNodeIndex;
    }

    features::SemanticTokensRequest request{uriStr, text, tree, m_symbolTable};
    request.scopeRoot = m_scopeIndex.GetRoot(uriStr);
    request.nodeIndex = nodeIndexPtr;
    request.excludedLineRanges = ExcludedLineRanges(text);
    lsp::SemanticTokens tokens = features::GetSemanticTokens(request);
    codec::EncodeSemanticTokens(text, m_positionEncoding, tokens.data);

    const bool hasError = tree != nullptr && ts_node_has_error(ts_tree_root_node(tree));

    // Cached after encoding, so a delta is computed against exactly the bytes the client holds.
    const std::string resultId = std::to_string(++m_semanticTokensRevision);
    tokens.resultId = resultId;
    {
        std::lock_guard<std::mutex> lock(m_semanticTokensMutex);
        m_semanticTokensCache[uriStr] = SemanticTokensSnapshot{resultId, tokens.data, hasError, currentVersion};
    }

    return tokens;
}

void Server::BuildDiagnosticSeverityOverrides()
{
    m_diagnosticSeverities.clear();

    for (const auto& [code, severityName] : m_config.diagnosticSeverities)
    {
        if (severityName == "error")
            m_diagnosticSeverities[code] = angel_lsp::analysis::DiagnosticSeverity::Error;
        else if (severityName == "warning")
            m_diagnosticSeverities[code] = angel_lsp::analysis::DiagnosticSeverity::Warning;
        else if (severityName == "information")
            m_diagnosticSeverities[code] = angel_lsp::analysis::DiagnosticSeverity::Information;
        else if (severityName == "hint")
            m_diagnosticSeverities[code] = angel_lsp::analysis::DiagnosticSeverity::Hint;
        else
            LogError(fmt::format("Unknown diagnostic severity '{}' for '{}'; ignored", severityName, code));
    }

    if (!m_diagnosticSeverities.empty())
    {
        LogInfo(fmt::format("Diagnostic severity overrides active: {}", m_diagnosticSeverities.size()));
    }

    // Engine options are reported only when they differ from AngelScript's own defaults. They
    // change which diagnostics can appear at all, so a surprising silence is worth being able
    // to explain from the log; saying so on every start when nothing was set is just noise.
    LogNonDefaultEngineProperties();
}

void Server::LogNonDefaultEngineProperties() const
{
    const config::EngineProperties defaults;
    std::string changed;

    const auto note = [&changed](bool current, bool byDefault, std::string_view name)
    {
        if (current == byDefault)
        {
            return;
        }
        if (!changed.empty())
        {
            changed += ", ";
        }
        changed += name;
        changed += current ? "=true" : "=false";
    };

    note(m_config.engine.allowUnsafeReferences, defaults.allowUnsafeReferences, "allowUnsafeReferences");
    note(m_config.engine.privatePropAsProtected, defaults.privatePropAsProtected, "privatePropAsProtected");
    note(m_config.engine.disallowGlobalVars, defaults.disallowGlobalVars, "disallowGlobalVars");
    note(m_config.engine.foreachSupport, defaults.foreachSupport, "foreachSupport");
    note(m_config.engine.requireEnumScope, defaults.requireEnumScope, "requireEnumScope");
    note(m_config.engine.alwaysImplDefaultConstruct, defaults.alwaysImplDefaultConstruct, "alwaysImplDefaultConstruct");
    note(m_config.engine.allowUnicodeIdentifiers, defaults.allowUnicodeIdentifiers, "allowUnicodeIdentifiers");
    note(m_config.engine.ignoreDuplicateSharedIntf, defaults.ignoreDuplicateSharedIntf, "ignoreDuplicateSharedIntf");
    if (m_config.engine.compilerWarnings != defaults.compilerWarnings)
    {
        if (!changed.empty())
        {
            changed += ", ";
        }
        changed += "compilerWarnings=" + std::to_string(m_config.engine.compilerWarnings);
    }

    if (!changed.empty())
    {
        LogInfo(fmt::format("Engine properties differing from the defaults: {}", changed));
    }
}

std::string Server::CanonicalPathFromUri(const std::string& uriStr)
{
    const lsp::Uri uri = lsp::Uri::parse(uriStr);
    if (!uri.isValid() || !uri.isFileUri())
        return "";

    return angel_lsp::utils::IncludeResolver::NormalizePath(uri.fsPath());
}

std::string Server::DocumentKey(const std::string& uriStr)
{
    const std::string path = CanonicalPathFromUri(uriStr);
    return path.empty() ? uriStr : UriFromPath(path);
}

std::string Server::UriFromPath(const std::string& path)
{
    return angel_lsp::utils::PathToUri(angel_lsp::utils::IncludeResolver::NormalizePath(path));
}

const std::string* Server::FindDocumentText(const std::string& uri) const
{
    const std::string key = DocumentKey(uri);

    if (angel_lsp::utils::IsPredefinedFile(key, m_config.info.predefinedFileExtension))
    {
        if (const std::string* predefined = m_predefinedManager.GetDocumentTextPtr(key))
        {
            return predefined;
        }
    }

    if (const std::string* open = m_documentStore.GetTextPtr(key))
    {
        return open;
    }

    // Closure files are not open, but their ranges still reach the client through references,
    // definitions and multi-file rename edits, so their text has to be reachable too.
    if (const auto closure = m_closureDocuments.find(key); closure != m_closureDocuments.end())
    {
        return &closure->second;
    }

    return nullptr;
}

std::optional<Server::OpenDocument> Server::LookupOpenDocument(const std::string& uriStr)
{
    const std::string key = DocumentKey(uriStr);

    auto doc = m_documentStore.GetDocument(key);
    if (!doc)
    {
        if (key.starts_with("angelscript-virtual:") || uriStr.starts_with("angelscript-virtual:"))
        {
            std::string text = GenerateVirtualMixinDocument(key.starts_with("angelscript-virtual:") ? key : uriStr);
            if (!text.empty())
            {
                document::TreePtr tree =
                    m_parser ? document::MakeTreePtr(m_parser->Parse(text)) : document::MakeTreePtr(nullptr);
                m_documentStore.OpenDocument(DocumentStore::OpenDocumentRequest{key, text, 0, std::move(tree), key});
                doc = m_documentStore.GetDocument(key);
            }
            else
            {
                return std::nullopt;
            }
        }
        else
        {
            return std::nullopt;
        }
    }

    const std::string* textPtr = doc ? &doc->text : nullptr;
    TSTree* treePtr = doc ? doc->tree.get() : nullptr;
    std::shared_ptr<const std::string> predefinedHandle;

    if (angel_lsp::utils::IsPredefinedFile(key, m_config.info.predefinedFileExtension))
    {
        predefinedHandle = m_predefinedManager.GetDocumentTextShared(key);
        if (predefinedHandle)
        {
            textPtr = predefinedHandle.get();
        }
    }

    return OpenDocument{key, textPtr, treePtr, doc, predefinedHandle};
}

void Server::InitHandles()
{
    RegisterWorkspaceHandlers();
    RegisterTextDocumentHandlers();
    RegisterHierarchyHandlers();
    RegisterTokensAndFormattingHandlers();
}
} // namespace angel_lsp