#include "Server.h"
#include "utils/Utils.h"
#include "utils/PreprocessorRegions.h"
#include "utils/WorkspaceScan.h"
#include "utils/Constants.h"
#include "utils/Timer.h"
#include "lsp/PositionCodec.h"
#include "features/hover/HoverHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/implementation/ImplementationHandler.h"
#include "features/call_hierarchy/CallHierarchyHandler.h"
#include "features/type_hierarchy/TypeHierarchyHandler.h"
#include "features/linked_editing/LinkedEditingRangeHandler.h"
#include "features/selection_range/SelectionRangeHandler.h"
#include "features/completion/CompletionHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "features/signature_help/SignatureHelpHandler.h"
#include "features/document_symbol/DocumentSymbolHandler.h"
#include "features/workspace_symbol/WorkspaceSymbolHandler.h"
#include "features/references/ReferencesHandler.h"
#include "features/rename/RenameHandler.h"
#include "features/document_highlight/DocumentHighlightHandler.h"
#include "features/folding_range/FoldingRangeHandler.h"
#include "features/inlay_hint/InlayHintHandler.h"
#include "features/code_action/CodeActionHandler.h"
#include "features/formatting/FormattingHandler.h"
#include "features/formatting/PredefinedStubFormatter.h"
#include "features/document_link/DocumentLinkHandler.h"
#include "features/code_lens/CodeLensHandler.h"
#include "analysis/EngineProfiles.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <optional>
#include <variant>
#include <spdlog/fmt/fmt.h>

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
            case analysis::DiagnosticSeverity::Error:       return lsp::DiagnosticSeverity::Error;
            case analysis::DiagnosticSeverity::Warning:     return lsp::DiagnosticSeverity::Warning;
            case analysis::DiagnosticSeverity::Information: return lsp::DiagnosticSeverity::Information;
            case analysis::DiagnosticSeverity::Hint:        return lsp::DiagnosticSeverity::Hint;
            }
            return lsp::DiagnosticSeverity::Error;
        }
    }

    Server::Server(const angel_lsp::config::ServerConfig &config, lsp::io::Stream &stream)
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

        m_analysisThread = std::thread([this] { this->RunAnalysisLoop(); });
    }

    Server::~Server()
    {
        m_running = false;

        // Stopped and joined before the trees below are freed: both threads read member state and
        // must not outlive it. A destructor that joined implicitly would do so in reverse
        // declaration order - and m_workspaceThread is declared early enough that it would be
        // joined only after the symbol table and include graph it reads were already gone.
        {
            std::lock_guard<std::mutex> lock(m_analysisMutex);
            m_analysisStop = true;
        }
        m_analysisCv.notify_all();
        if (m_analysisThread.joinable())
            m_analysisThread.join();

        m_workspaceStop.Request();
        if (m_workspaceThread.joinable())
            m_workspaceThread.join();

        m_documentTrees.clear();
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

    bool Server::SetDefinedWordsFrom(const std::string &source, std::vector<std::string> words)
    {
        auto merged = std::make_shared<ankerl::unordered_dense::set<std::string>>();

        std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);

        if (words.empty())
            m_definedWordsBySource.erase(source);
        else
            m_definedWordsBySource[source] = std::move(words);

        for (const auto &[_, contributed] : m_definedWordsBySource)
        {
            for (const auto &word : contributed)
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
            catch (const lsp::ConnectionError &e)
            {
                LogInfo(fmt::format("Connection closed: {}", e.what()));
                m_running = false;
                continue;
            }
            catch (const lsp::io::Error &e)
            {
                LogInfo(fmt::format("Transport closed: {}", e.what()));
                m_running = false;
                continue;
            }
            catch (const lsp::json::ParseError &e)
            {
                LogError(fmt::format("Malformed JSON-RPC message discarded: {}", e.what()));
            }
            catch (const lsp::jsonrpc::ProtocolError &e)
            {
                LogError(fmt::format("Protocol error, message discarded: {}", e.what()));
            }
            catch (const std::exception &e)
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
                LogError(fmt::format("Giving up after {} consecutive message errors; closing the session.",
                                               consecutiveErrors));
                m_running = false;
            }
        }
    }

    lsp::requests::Initialize::Result Server::HandleRequestsInitialized(lsp::requests::Initialize::Params &&params)
    {
        if (params.workspaceFolders.has_value() && !params.workspaceFolders.value().isNull())
        {
            for (const auto &workspace : params.workspaceFolders.value().value())
            {
                std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
                const std::string fsPath = workspace.uri.fsPath();
                if (!fsPath.empty())
                    m_workspacesRoot.push_back(angel_lsp::utils::IncludeResolver::NormalizePath(fsPath));
            }
        }

        if (params.initializationOptions.has_value() && params.initializationOptions->isObject())
        {
            const lsp::LSPObject *initSection = &params.initializationOptions->object();
            if (const auto *nested = initSection->find("angelscript"); nested && nested->isObject())
            {
                initSection = &nested->object();
            }
            const lsp::LSPObject *engineObj = nullptr;
            if (const auto *e = initSection->find("engine"); e && e->isObject())
            {
                engineObj = &e->object();
            }

            auto getInitBool = [&](std::string_view name) -> std::optional<bool> {
                if (engineObj)
                {
                    if (const auto *val = engineObj->find(std::string(name)); val && val->isBoolean())
                        return val->boolean();
                }
                std::string dotKey = "engine." + std::string(name);
                if (const auto *val = initSection->find(dotKey); val && val->isBoolean())
                    return val->boolean();
                std::string fullKey = "angelscript.engine." + std::string(name);
                if (const auto *val = initSection->find(fullKey); val && val->isBoolean())
                    return val->boolean();
                return std::nullopt;
            };

            auto getInitInt = [&](std::string_view name) -> std::optional<int> {
                if (engineObj)
                {
                    if (const auto *val = engineObj->find(std::string(name)); val && val->isNumber())
                        return static_cast<int>(val->number());
                }
                std::string dotKey = "engine." + std::string(name);
                if (const auto *val = initSection->find(dotKey); val && val->isNumber())
                    return static_cast<int>(val->number());
                std::string fullKey = "angelscript.engine." + std::string(name);
                if (const auto *val = initSection->find(fullKey); val && val->isNumber())
                    return static_cast<int>(val->number());
                return std::nullopt;
            };

            if (auto v = getInitBool("foreachSupport")) m_config.engine.foreachSupport = *v;
            if (auto v = getInitBool("requireEnumScope")) m_config.engine.requireEnumScope = *v;
            if (auto v = getInitBool("alwaysImplDefaultConstruct")) m_config.engine.alwaysImplDefaultConstruct = *v;
            if (auto v = getInitBool("allowUnicodeIdentifiers")) m_config.engine.allowUnicodeIdentifiers = *v;
            if (auto v = getInitBool("ignoreDuplicateSharedIntf")) m_config.engine.ignoreDuplicateSharedIntf = *v;
            if (auto v = getInitInt("compilerWarnings")) m_config.engine.compilerWarnings = *v;
        }

        m_i18n = std::make_unique<angel_lsp::i18n::I18n>(params.locale.value_or(m_config.info.locale.empty() ? "en" : m_config.info.locale));

        lsp::requests::Initialize::Result result;

        lsp::ServerInfo info;
        info.name = m_config.info.name;
        info.version = m_config.info.version;
        result.serverInfo = info;

        // Position encoding negotiation. Tree-sitter reports columns in bytes, so UTF-8 lets every
        // conversion in utils/PositionEncoding.h short-circuit to an identity. UTF-16 is the
        // protocol default and the only encoding a server may assume when the client offers none,
        // so that is what we fall back to - and then every position crossing this boundary has to
        // be converted (see EncodeRange / DecodePosition below).
        m_positionEncoding = angel_lsp::utils::PositionEncoding::Utf16;
        if (params.capabilities.general.has_value() && params.capabilities.general->positionEncodings.has_value())
        {
            for (const auto &offered : params.capabilities.general->positionEncodings.value())
            {
                if (offered == lsp::PositionEncodingKind::UTF8)
                {
                    m_positionEncoding = angel_lsp::utils::PositionEncoding::Utf8;
                    break;
                }
            }
        }

        if (params.capabilities.window.has_value() &&
            params.capabilities.window->workDoneProgress.has_value())
        {
            m_workDoneProgressSupport = params.capabilities.window->workDoneProgress.value();
        }

        // Which of the two diagnostic models this client wants. See m_clientPullsDiagnostics.
        m_clientPullsDiagnostics = params.capabilities.textDocument.has_value() &&
                                   params.capabilities.textDocument->diagnostic.has_value();

        m_clientSupportsDiagnosticRefresh = params.capabilities.workspace.has_value() &&
                                            params.capabilities.workspace->diagnostics.has_value() &&
                                            params.capabilities.workspace->diagnostics->refreshSupport.has_value() &&
                                            params.capabilities.workspace->diagnostics->refreshSupport.value();

        if (params.capabilities.textDocument.has_value() &&
            params.capabilities.textDocument->completion.has_value() &&
            params.capabilities.textDocument->completion->completionItem.has_value() &&
            params.capabilities.textDocument->completion->completionItem->snippetSupport.has_value())
        {
            m_snippetSupport = params.capabilities.textDocument->completion->completionItem->snippetSupport.value();
        }

        const bool useUtf8 = m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8;
        result.capabilities.positionEncoding = useUtf8 ? lsp::PositionEncodingKind::UTF8
                                                       : lsp::PositionEncodingKind::UTF16;
        LogInfo(fmt::format("Negotiated position encoding: {}", useUtf8 ? "utf-8" : "utf-16"));
        m_symbolTable.SetVirtualMixinDocumentsEnabled(m_config.features.enableVirtualMixinDocuments);

        lsp::TextDocumentSyncOptions sync;
        sync.openClose = true;
        sync.change = lsp::TextDocumentSyncKind::Incremental;
        sync.willSave = true;
        sync.willSaveWaitUntil = true;

        lsp::SaveOptions saveOptions;
        saveOptions.includeText = true;
        sync.save = saveOptions;

        result.capabilities.textDocumentSync = sync;

        if (m_config.features.enableHover)
        {
            result.capabilities.hoverProvider = true;
        }

        if (m_config.features.enableDefinition)
        {
            result.capabilities.definitionProvider = true;
            result.capabilities.typeDefinitionProvider = true;

            // AngelScript has no declaration/definition split - no headers, no prototypes, which
            // is the whole reason as-err-missing-body exists - so "Go to Declaration" is the same
            // question as "Go to Definition" and is answered by the same handler. Announcing it
            // costs nothing and stops the editor's second navigation key doing nothing at all.
            result.capabilities.declarationProvider = true;

            // A moniker is the same lookup as a definition, so it lives and dies with that
            // switch rather than getting one of its own.
            result.capabilities.monikerProvider = true;
        }

        if (m_config.features.enableImplementation)
        {
            result.capabilities.implementationProvider = true;
        }

        if (m_config.features.enableSelectionRange)
        {
            result.capabilities.selectionRangeProvider = true;
        }

        if (m_config.features.enableCallHierarchy)
        {
            result.capabilities.callHierarchyProvider = true;
        }

        if (m_config.features.enableTypeHierarchy)
        {
            result.capabilities.typeHierarchyProvider = true;
        }

        if (m_config.features.enableLinkedEditing)
        {
            result.capabilities.linkedEditingRangeProvider = true;
        }

        if (m_config.features.enableCodeLens)
        {
            lsp::CodeLensOptions codeLensOpts;
            codeLensOpts.resolveProvider = true;
            result.capabilities.codeLensProvider = codeLensOpts;
        }

        if (m_config.features.enableCompletion)
        {
            lsp::CompletionOptions completionOpts;
            // The spec requires single characters: a two-character "::" never fires, and AngelScript
            // has no "->" operator at all. Typing the first ":" of "::" is what has to trigger.
            completionOpts.triggerCharacters = lsp::Array<lsp::String>{ ".", ":" };
            // Documentation is attached on demand: reading the doc comment above a declaration
            // means finding its file and re-scanning lines, and a completion list is hundreds of
            // items of which the user reads one.
            completionOpts.resolveProvider = true;
            result.capabilities.completionProvider = completionOpts;
        }

        if (m_config.features.enableSemanticTokens)
        {
            lsp::SemanticTokensOptions semOpts;
            semOpts.legend = features::GetSemanticTokensLegend();
            // Delta rather than a plain full: the payload is five integers per token, and a typing
            // session would otherwise re-send every one of them on each keystroke.
            lsp::SemanticTokensFullDelta fullDelta;
            fullDelta.delta = true;
            semOpts.full = fullDelta;
            // Lets the editor ask for just the visible viewport instead of the whole file, which is
            // the difference between re-tokenising a 3000-line script and re-tokenising 50 lines.
            semOpts.range = true;
            result.capabilities.semanticTokensProvider = semOpts;
        }

        if (m_config.features.enableSignatureHelp)
        {
            lsp::SignatureHelpOptions sigOpts;
            sigOpts.triggerCharacters = lsp::Array<lsp::String>{ "(", "," };
            result.capabilities.signatureHelpProvider = sigOpts;
        }

        if (m_config.features.enableDocumentSymbols)
        {
            result.capabilities.documentSymbolProvider = true;
        }

        if (m_config.features.enableWorkspaceSymbols)
        {
            lsp::WorkspaceSymbolOptions wsOpts;
            wsOpts.resolveProvider = true;
            result.capabilities.workspaceSymbolProvider = wsOpts;
        }

        if (m_config.features.enableReferences)
        {
            result.capabilities.referencesProvider = true;
        }

        if (m_config.features.enableRename)
        {
            lsp::RenameOptions renameOpts;
            renameOpts.prepareProvider = true;
            result.capabilities.renameProvider = renameOpts;
        }

        if (m_config.features.enableDocumentHighlight)
        {
            result.capabilities.documentHighlightProvider = true;
        }

        if (m_config.features.enableFoldingRange)
        {
            result.capabilities.foldingRangeProvider = true;
        }

        if (m_config.features.enableInlayHints)
        {
            lsp::InlayHintOptions inlayOpts;
            inlayOpts.resolveProvider = true;
            result.capabilities.inlayHintProvider = inlayOpts;
        }

        if (m_config.features.enableCodeAction)
        {
            lsp::CodeActionOptions codeActionOpts;
            codeActionOpts.resolveProvider = true;
            codeActionOpts.codeActionKinds = lsp::Array<lsp::CodeActionKindEnum>{
                lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix),
                lsp::CodeActionKindEnum(lsp::CodeActionKind::Refactor),
                lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract),
                lsp::CodeActionKindEnum(lsp::CodeActionKind::SourceOrganizeImports)
            };
            result.capabilities.codeActionProvider = codeActionOpts;
        }

        if (m_config.features.enableFormatting)
        {
            result.capabilities.documentFormattingProvider = true;
            lsp::DocumentRangeFormattingOptions rangeOpts;
            rangeOpts.rangesSupport = true;
            result.capabilities.documentRangeFormattingProvider = rangeOpts;
        }

        if (m_config.features.enableOnTypeFormatting)
        {
            lsp::DocumentOnTypeFormattingOptions onTypeOpts;
            onTypeOpts.firstTriggerCharacter = ";";
            onTypeOpts.moreTriggerCharacter = lsp::Array<lsp::String>{ "}", "\n" };
            result.capabilities.documentOnTypeFormattingProvider = onTypeOpts;
        }

        if (m_config.features.enableDocumentLink)
        {
            lsp::DocumentLinkOptions linkOpts;
            linkOpts.resolveProvider = true;
            result.capabilities.documentLinkProvider = linkOpts;
        }

        // Pull diagnostics alongside the push ones, not instead of them. A client that supports
        // pull uses it and ignores the notifications; one that does not never sends the request.
        // Announcing both is what lets the same server serve either.
        //
        // interFileDependencies is true and it is not a formality: an `#include` changes the
        // diagnostics of every file including it, which is exactly the case the flag exists for.
        // workspaceDiagnostics reports what has already been analysed - see the handler.
        if (m_config.features.enablePullDiagnostics)
        {
            lsp::DiagnosticOptions diagnosticOpts;
            diagnosticOpts.identifier = std::string("angelscript");
            diagnosticOpts.interFileDependencies = true;
            diagnosticOpts.workspaceDiagnostics = true;
            result.capabilities.diagnosticProvider = diagnosticOpts;
        }

        // Announced unconditionally: both the include graph and the predefined-stub scan are scoped
        // to the known roots, so a folder added mid-session has to reach the server whatever else
        // is switched off.
        lsp::WorkspaceFoldersServerCapabilities folderCaps;
        folderCaps.supported = true;
        folderCaps.changeNotifications = true;

        lsp::WorkspaceOptions workspaceOpts;
        workspaceOpts.workspaceFolders = folderCaps;

        // Renames and deletions of script files. Announced with a filter so the editor does not
        // wake this server for every file in the repository - it is only ever interested in the
        // ones the include graph can hold.
        //
        // The three `did` operations. The `will` variants are REQUESTS, and answering one blocks
        // the rename in the editor until the server replies. A rename that pauses because a
        // language server is thinking is a worse experience than one whose #include fixup arrives a
        // moment later, and nothing here needs to veto the operation.
        {
            lsp::FileOperationPattern scriptPattern;
            scriptPattern.glob = fmt::format("**/*{}", m_config.info.fileExtension);

            lsp::FileOperationFilter scriptFilter;
            scriptFilter.pattern = scriptPattern;
            scriptFilter.scheme = std::string("file");

            lsp::FileOperationRegistrationOptions registration;
            registration.filters = lsp::Array<lsp::FileOperationFilter>{ scriptFilter };

            lsp::FileOperationOptions fileOps;
            fileOps.didCreate = registration;
            fileOps.didRename = registration;
            fileOps.didDelete = registration;
            workspaceOpts.fileOperations = fileOps;
        }

        // Read-only virtual documents under one scheme, so a user can open the predefined stub their
        // workspace is analysed against. It often lives outside the workspace and is otherwise
        // unopenable, which makes every "unknown type" impossible to check by hand.
        {
            lsp::TextDocumentContentOptions contentOpts;
            lsp::Array<lsp::String> schemes{ "angelscript-predefined" };
            if (m_config.features.enableVirtualMixinDocuments)
            {
                schemes.push_back("angelscript-virtual");
            }
            contentOpts.schemes = schemes;
            workspaceOpts.textDocumentContent = contentOpts;
        }

        result.capabilities.workspace = workspaceOpts;

        lsp::ExecuteCommandOptions cmdOpts;
        cmdOpts.commands = lsp::Array<lsp::String>{ "angelscript.rescanWorkspace",
                                                    "angelscript.listPredefinedStubs" };
        result.capabilities.executeCommandProvider = cmdOpts;

        return result;
    }

    void Server::HandleNotificationsInitialized(lsp::notifications::Initialized::Params &&/*params*/)
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


    lsp::SemanticTokens Server::ComputeAndCacheSemanticTokens(const std::string &uriStr, const std::string &text)
    {
        TSTree *tree = nullptr;
        if (auto it = m_documentTrees.find(uriStr); it != m_documentTrees.end())
        {
            tree = it->second.get();
        }

        int currentVersion = -1;
        if (auto it = m_documentVersions.find(uriStr); it != m_documentVersions.end())
        {
            currentVersion = it->second;
        }

        features::SemanticTokensRequest request{ uriStr, text, tree, m_symbolTable, m_scopeIndex.GetRoot(uriStr) };
        request.excludedLineRanges = ExcludedLineRanges(text);
        lsp::SemanticTokens tokens = features::GetSemanticTokens(request);
        codec::EncodeSemanticTokens(text, m_positionEncoding, tokens.data);

        const bool hasError = tree != nullptr && ts_node_has_error(ts_tree_root_node(tree));

        // Cached after encoding, so a delta is computed against exactly the bytes the client holds.
        const std::string resultId = std::to_string(++m_semanticTokensRevision);
        tokens.resultId = resultId;
        {
            std::lock_guard<std::mutex> lock(m_semanticTokensMutex);
            m_semanticTokensCache[uriStr] = SemanticTokensSnapshot{ resultId, tokens.data, hasError, currentVersion };
        }

        return tokens;
    }


    void Server::BuildDiagnosticSeverityOverrides()
    {
        m_diagnosticSeverities.clear();

        for (const auto &[code, severityName] : m_config.diagnosticSeverities)
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


    std::string Server::CanonicalPathFromUri(const std::string &uriStr)
    {
        const lsp::Uri uri = lsp::Uri::parse(uriStr);
        if (!uri.isValid() || !uri.isFileUri())
            return "";

        return angel_lsp::utils::IncludeResolver::NormalizePath(uri.fsPath());
    }

    std::string Server::DocumentKey(const std::string &uriStr)
    {
        const std::string path = CanonicalPathFromUri(uriStr);
        return path.empty() ? uriStr : UriFromPath(path);
    }

    std::string Server::UriFromPath(const std::string &path)
    {
        return angel_lsp::utils::PathToUri(angel_lsp::utils::IncludeResolver::NormalizePath(path));
    }


    const std::string *Server::FindDocumentText(const std::string &uri) const
    {
        const std::string key = DocumentKey(uri);

        if (angel_lsp::utils::IsPredefinedFile(key, m_config.info.predefinedFileExtension))
        {
            std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(m_predefinedMutex));
            if (const auto predefined = m_predefinedDocuments.find(key); predefined != m_predefinedDocuments.end())
            {
                return &predefined->second;
            }
        }

        if (const auto open = m_openDocuments.find(key); open != m_openDocuments.end())
        {
            return &open->second;
        }

        // Closure files are not open, but their ranges still reach the client through references,
        // definitions and multi-file rename edits, so their text has to be reachable too.
        if (const auto closure = m_closureDocuments.find(key); closure != m_closureDocuments.end())
        {
            return &closure->second;
        }

        return nullptr;
    }


    std::optional<Server::OpenDocument> Server::LookupOpenDocument(const std::string &uriStr)
    {
        const std::string key = DocumentKey(uriStr);

        auto docIt = m_openDocuments.find(key);
        if (docIt == m_openDocuments.end())
        {
            if (key.starts_with("angelscript-virtual:") || uriStr.starts_with("angelscript-virtual:"))
            {
                std::string text = GenerateVirtualMixinDocument(key.starts_with("angelscript-virtual:") ? key : uriStr);
                if (!text.empty())
                {
                    m_openDocuments[key] = text;
                    if (m_parser)
                    {
                        m_documentTrees.insert_or_assign(key, document::MakeTreePtr(m_parser->Parse(text)));
                    }
                    docIt = m_openDocuments.find(key);
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

        const auto treeIt = m_documentTrees.find(key);

        const std::string *textPtr = &docIt->second;
        if (angel_lsp::utils::IsPredefinedFile(key, m_config.info.predefinedFileExtension))
        {
            std::lock_guard<std::mutex> lock(m_predefinedMutex);
            auto preIt = m_predefinedDocuments.find(key);
            if (preIt != m_predefinedDocuments.end())
            {
                textPtr = &preIt->second;
            }
        }

        return OpenDocument{ key, textPtr,
                             treeIt == m_documentTrees.end() ? nullptr : treeIt->second.get() };
    }


    void Server::InitHandles()
    {
        RegisterWorkspaceHandlers();
        RegisterTextDocumentHandlers();
        RegisterHierarchyHandlers();
        RegisterTokensAndFormattingHandlers();
    }
}