#include "Server.h"
#include "utils/Utils.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Constants.h"
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
#include "features/document_link/DocumentLinkHandler.h"
#include "features/code_lens/CodeLensHandler.h"
#include "analysis/EngineProfiles.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
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
        m_logger->SetLevel(angel_lsp::utils::ParseLogLevel(m_config.info.logLevel, angel_lsp::utils::LogLevel::Info));

        m_parser = std::make_unique<angel_lsp::parser::AngelScriptParser>(m_logger.get());

        m_symbolCollector = std::make_unique<angel_lsp::analysis::SymbolCollector>(m_logger.get());

        m_localScopeCollector = std::make_unique<angel_lsp::analysis::LocalScopeCollector>(m_logger.get());

        m_semanticAnalyzer = std::make_unique<angel_lsp::analysis::SemanticAnalyzer>(m_logger.get());

        m_i18n = std::make_unique<angel_lsp::i18n::I18n>(m_config.info.locale.empty() ? "en" : m_config.info.locale);

        // Seeded from the startup config and mutable from here on. Everything that reads these
        // three goes through the accessors below; m_config's own copies are not read again.
        m_searchDirectories = std::make_shared<const std::vector<std::string>>(m_config.searchDirectories);
        m_engineProfile = m_config.engineProfile;
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

        for (auto &[uri, tree] : m_documentTrees)
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }
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

    std::vector<std::string> Server::IncludeAllowedRoots() const
    {
        std::vector<std::string> roots;

        for (const auto &workspaceRoot : WorkspaceRoots())
        {
            std::string path = angel_lsp::utils::UriToPath(workspaceRoot);
            if (!path.empty())
                roots.push_back(std::move(path));
        }

        const auto searchDirectories = SearchDirectories();
        roots.insert(roots.end(), searchDirectories->begin(), searchDirectories->end());

        // A configured stub normally ships with the host application, outside every workspace
        // folder. Its own directory is therefore legitimately includable even though nothing else
        // there is - so the parent is added, not the whole tree above it.
        for (const auto &predefined : m_config.predefinedFiles)
        {
            std::error_code ec;
            std::filesystem::path configured(predefined);
            if (configured.has_parent_path())
                roots.push_back(configured.parent_path().string());
        }

        return roots;
    }

    std::vector<angel_lsp::utils::ExcludedLineRange> Server::ExcludedLineRanges(const std::string &text) const
    {
        // Built per call rather than cached: it is one linear scan of the document, next to nothing
        // beside the parse and analysis it accompanies, and caching it would mean invalidating it on
        // every edit for no measurable gain.
        ankerl::unordered_dense::set<std::string> defined;
        defined.reserve(m_config.definedWords.size());
        for (const auto &word : m_config.definedWords)
            defined.insert(word);

        return angel_lsp::utils::FindExcludedLineRanges(text, defined);
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
                m_logger->LogInfo(fmt::format("Connection closed: {}", e.what()));
                m_running = false;
                continue;
            }
            catch (const lsp::io::Error &e)
            {
                m_logger->LogInfo(fmt::format("Transport closed: {}", e.what()));
                m_running = false;
                continue;
            }
            catch (const lsp::json::ParseError &e)
            {
                m_logger->LogError(fmt::format("Malformed JSON-RPC message discarded: {}", e.what()));
            }
            catch (const lsp::jsonrpc::ProtocolError &e)
            {
                m_logger->LogError(fmt::format("Protocol error, message discarded: {}", e.what()));
            }
            catch (const std::exception &e)
            {
                // A bug in one handler is not a reason to drop the session. The transport wraps
                // everything it does not recognise into ConnectionError, so anything arriving here
                // came from message dispatch, not from the stream.
                m_logger->LogError(fmt::format("Unhandled exception handling message: {}", e.what()));
            }

            // Guard against a stream that fails the same way forever - recovering from a frame we
            // cannot consume would spin this loop at full tilt with no way out.
            if (++consecutiveErrors >= k_maxConsecutiveMessageErrors)
            {
                m_logger->LogError(fmt::format("Giving up after {} consecutive message errors; closing the session.",
                                               consecutiveErrors));
                m_running = false;
            }
        }
    }

    auto Server::HandleRequestsInitialized(lsp::requests::Initialize::Params &&params)
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
        m_logger->LogInfo(fmt::format("Negotiated position encoding: {}", useUtf8 ? "utf-8" : "utf-16"));

        lsp::TextDocumentSyncOptions sync;
        sync.openClose = true;
        sync.change = lsp::TextDocumentSyncKind::Incremental;

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
            result.capabilities.workspaceSymbolProvider = true;
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
            result.capabilities.inlayHintProvider = true;
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
            result.capabilities.documentRangeFormattingProvider = true;
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
            linkOpts.resolveProvider = false;
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

        result.capabilities.workspace = workspaceOpts;

        return result;
    }

    void Server::HandleNotificationsInitialized(lsp::notifications::Initialized::Params &&/*params*/)
    {
        // Started unconditionally: even with the predefined-stub loader disabled, the workspace
        // thread still has to build the #include graph that module-closure indexing depends on.
        StartWorkspaceScan();
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

        BeginWorkspaceProgress("AngelScript: indexing workspace");
        ReportWorkspaceProgress("Building the include graph", 0);

        m_includeGraph.Build(roots,
                             *searchDirectories,
                             m_config.info.fileExtension,
                             [&stopToken]() { return stopToken.stop_requested(); },
                             {},
                             m_config.exclude);

        if (stopToken.stop_requested())
        {
            EndWorkspaceProgress("Cancelled");
            return;
        }

        m_logger->LogInfo(fmt::format("Include graph built: {} script file(s)", m_includeGraph.FileCount()));
        ReportWorkspaceProgress(
            fmt::format("Indexed {} script file(s)", m_includeGraph.FileCount()), 40);

        if (!m_config.features.enablePredefinedLoader)
        {
            EndWorkspaceProgress(fmt::format("{} script file(s)", m_includeGraph.FileCount()));
            return;
        }

        angel_lsp::parser::AngelScriptParser backgroundParser(m_logger.get());

        // Built-in predefined engine profiles (e.g. Standard, SvenCoop, Urho3D, OpenXRay, OOTP)
        ReportWorkspaceProgress("Loading engine profiles", 55);
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
        ReportWorkspaceProgress("Loading predefined stubs", 70);
        LoadConfiguredPredefinedFiles(backgroundParser, stopToken);

        if (stopToken.stop_requested())
        {
            EndWorkspaceProgress("Cancelled");
            return;
        }

        try
        {
            for (const auto &workspaceRoot : workspaceRoots)
            {
                if (stopToken.stop_requested())
                    return;

                // skip_permission_denied, matching WorkspaceIncludeGraph::Build. Without it a
                // single unreadable directory anywhere under a workspace root threw, and the catch
                // below abandoned the whole predefined-stub scan - so one permission-protected
                // folder silently cost the user every stub in the workspace.
                std::error_code scanError;
                std::filesystem::recursive_directory_iterator scan(
                    angel_lsp::utils::UriToPath(workspaceRoot),
                    std::filesystem::directory_options::skip_permission_denied,
                    scanError);
                const std::filesystem::recursive_directory_iterator scanEnd;
                for (; scan != scanEnd; ++scan)
                {
                    if (stopToken.stop_requested())
                    {
                        EndWorkspaceProgress("Cancelled");
                        return;
                    }

                    const auto &entry = *scan;

                    // Pruned, not filtered: disable_recursion_pending stops the walk from entering
                    // the directory at all, where a filter would still visit every file inside it.
                    std::error_code dirError;
                    if (entry.is_directory(dirError) &&
                        angel_lsp::utils::IsExcludedDirectory(entry.path().generic_string(), m_config.exclude))
                    {
                        scan.disable_recursion_pending();
                        continue;
                    }

                    if (entry.exists() && entry.is_regular_file())
                    {
                        if (angel_lsp::utils::IsPredefinedFile(entry.path().string(), m_config.info.predefinedFileExtension))
                            ParserPredefined(entry.path().string(), backgroundParser);
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            m_logger->LogError(fmt::format("Error reading workspace files: {}", e.what()));
        }

        EndWorkspaceProgress(fmt::format("{} script file(s) indexed", m_includeGraph.FileCount()));
    }

    void Server::LoadBuiltinEngineProfiles(angel_lsp::parser::AngelScriptParser &parser,
                                           const angel_lsp::utils::StopFlag &stopToken)
    {
        if (!m_config.features.enablePredefinedLoader)
        {
            return;
        }

        // By value, not by reference into m_config: this runs on the workspace thread and
        // didChangeConfiguration can rewrite the profile from the message loop mid-call.
        const std::string profileName = EngineProfile();
        auto kind = angel_lsp::analysis::ParseEngineProfileKind(profileName);
        if (kind == angel_lsp::analysis::EngineProfileKind::None)
        {
            return;
        }

        if (kind == angel_lsp::analysis::EngineProfileKind::Auto)
        {
            std::vector<std::string> fileNames;
            for (const auto &workspaceRoot : WorkspaceRoots())
            {
                if (stopToken.stop_requested())
                    return;

                const std::string rootPath = angel_lsp::utils::UriToPath(workspaceRoot);
                std::error_code ec;

                // skip_permission_denied and a stop check per entry, matching every other scan in
                // this file. This was the one walk with neither: an unreadable directory ended it
                // early with no profile detected, and because RestartWorkspaceScan joins this thread
                // from the message loop, a large tree here was the one place that join could
                // actually be felt as a pause.
                std::filesystem::recursive_directory_iterator scan(
                    rootPath, std::filesystem::directory_options::skip_permission_denied, ec);
                const std::filesystem::recursive_directory_iterator scanEnd;
                for (; scan != scanEnd; ++scan)
                {
                    if (stopToken.stop_requested())
                        return;

                    const auto &entry = *scan;

                    std::error_code dirError;
                    if (entry.is_directory(dirError) &&
                        angel_lsp::utils::IsExcludedDirectory(entry.path().generic_string(), m_config.exclude))
                    {
                        scan.disable_recursion_pending();
                        continue;
                    }

                    if (entry.is_regular_file())
                    {
                        fileNames.push_back(entry.path().filename().string());
                    }
                }
            }
            kind = angel_lsp::analysis::DetectEngineProfileFromWorkspace(fileNames);
        }

        std::vector<angel_lsp::analysis::EngineProfileKind> profilesToLoad;
        if (kind != angel_lsp::analysis::EngineProfileKind::Standard &&
            kind != angel_lsp::analysis::EngineProfileKind::None)
        {
            profilesToLoad.push_back(angel_lsp::analysis::EngineProfileKind::Standard);
        }
        profilesToLoad.push_back(kind);

        for (auto pKind : profilesToLoad)
        {
            if (stopToken.stop_requested())
            {
                return;
            }

            const std::string_view stubSource = angel_lsp::analysis::GetProfileStubSource(pKind);
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

            ReplaceSymbolsFromSource(syntheticUri, std::string(stubSource), parser);

            m_scopeIndex.ClearDocument(syntheticUri);
            m_callGraph.ClearDocument(syntheticUri);
            m_scopeIndex.SetScopeTree(syntheticUri, m_localScopeCollector->CollectScopes(std::string(stubSource), parser));

            m_logger->LogInfo(fmt::format("Loaded built-in engine profile: {}", angel_lsp::analysis::EngineProfileKindToString(pKind)));
        }
    }

    void Server::LoadConfiguredPredefinedFiles(angel_lsp::parser::AngelScriptParser &parser,
                                               const angel_lsp::utils::StopFlag &stopToken)
    {
        for (const auto &entry : m_config.predefinedFiles)
        {
            if (stopToken.stop_requested())
                return;

            std::error_code ec;
            const std::filesystem::path configured(entry);

            // Absolute paths are taken at face value - that is the whole point of the option, since
            // the stub commonly lives outside every workspace folder. Relative ones are resolved
            // against each folder, matching how searchDirectories entries are treated.
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
                loaded = true;
                break;
            }

            if (!loaded)
            {
                m_logger->LogError(fmt::format("Configured predefined file not found: {}", entry));
            }
        }
    }

    bool Server::ClaimPredefinedFile(const std::string &uriStr, bool forceReload)
    {
        const std::string path = CanonicalPathFromUri(uriStr);

        // A stub whose path cannot be canonicalised has nothing to key on, so it falls back to
        // plain URI identity - still enough to keep the same spelling from loading twice.
        if (path.empty())
        {
            return m_predefinedUris.insert(uriStr).second || forceReload;
        }

        if (const auto owner = m_predefinedUriByPath.find(path); owner != m_predefinedUriByPath.end())
        {
            if (owner->second == uriStr)
            {
                // Already ours. Only a caller that knows the file changed on disk has a reason to
                // pay for collecting it again.
                return forceReload;
            }

            // Same file, different spelling. The previous copy has to go before the new one is
            // collected, or every declaration in the stub would exist twice in the symbol table.
            m_symbolTable.ClearDocumentSymbols(owner->second);
            m_scopeIndex.ClearDocument(owner->second);
            m_callGraph.ClearDocument(owner->second);
            m_predefinedUris.erase(owner->second);

            m_logger->LogInfo(fmt::format("Predefined file re-indexed under {} (was {})", uriStr, owner->second));
        }

        m_predefinedUriByPath[path] = uriStr;
        m_predefinedUris.insert(uriStr);
        return true;
    }

    void Server::ParserPredefined(const std::string &filePath, angel_lsp::parser::AngelScriptParser &parser, bool forceReload)
    {
        std::string uri = angel_lsp::utils::PathToUri(filePath);

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            m_logger->LogError(fmt::format("Cannot open predefined file: {}", filePath));
            return;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // Claim and collect under one lock: didOpen may be claiming the very same stub under the
        // client's URI spelling on the message loop, and a claim that lands mid-collect would purge
        // symbols this call is about to re-add.
        std::lock_guard<std::mutex> lock(m_predefinedMutex);

        if (!ClaimPredefinedFile(uri, forceReload))
        {
            return;
        }

        ReplaceSymbolsFromSource(uri, content, parser);

        m_scopeIndex.ClearDocument(uri);
        m_callGraph.ClearDocument(uri);
        m_scopeIndex.SetScopeTree(uri, m_localScopeCollector->CollectScopes(content, parser));

        m_logger->LogInfo(fmt::format("Loaded predefined file: {}", filePath));
    }

    auto Server::HandleRequestsShutdown()
    {
        m_running = false;
        return lsp::requests::Shutdown::Result{};
    }

    void Server::HandleNotificationsExit()
    {
        m_running = false;
    }

    void Server::HandleNotificationsWorkspace_DidChangeConfiguration(lsp::notifications::Workspace_DidChangeConfiguration::Params &&params)
    {
        // The client synchronises the whole "angelscript" section (see the LanguageClient's
        // synchronize.configurationSection), so settings arrives either as that section directly or
        // wrapped in an object keyed by it, depending on the client. Both shapes are accepted.
        if (!params.settings.isObject())
        {
            return;
        }

        const lsp::LSPObject *section = &params.settings.object();
        if (const auto *nested = section->find("angelscript"); nested && nested->isObject())
        {
            section = &nested->object();
        }

        bool shouldRescan = false;

        // Nested one level down, because the client sends it as `angelscript.format.braceStyle`.
        // Nothing to rescan: it changes only what the next format request produces.
        if (const auto *formatVal = section->find("format"); formatVal && formatVal->isObject())
        {
            if (const auto *styleVal = formatVal->object().find("braceStyle");
                styleVal && styleVal->isString())
            {
                const bool wantsKR = BraceStyleIsKR(styleVal->string());
                if (wantsKR != m_formatBraceStyleKR.exchange(wantsKR, std::memory_order_relaxed))
                {
                    m_logger->LogInfo(fmt::format("Format brace style changed to '{}'", styleVal->string()));
                }
            }
        }

        if (const auto *profileVal = section->find("engineProfile"); profileVal && profileVal->isString())
        {
            if (!profileVal->string().empty() && profileVal->string() != EngineProfile())
            {
                {
                    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
                    m_engineProfile = profileVal->string();
                }
                m_logger->LogInfo(fmt::format("Engine profile changed to '{}'; reloading predefineds", profileVal->string()));
                shouldRescan = true;
            }
        }

        const auto *directories = section->find("searchDirectories");
        if (directories && directories->isArray())
        {
            std::vector<std::string> updated;
            for (const auto &entry : directories->array())
            {
                if (entry.isString() && !entry.string().empty())
                    updated.push_back(entry.string());
            }

            if (updated != *SearchDirectories())
            {
                const size_t count = updated.size();

                // Swapped in as a whole new list rather than assigned into the old one: a worker
                // holding the previous handle keeps reading that revision safely until it is done,
                // and the old buffer is freed only when the last of them lets go.
                {
                    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
                    m_searchDirectories = std::make_shared<const std::vector<std::string>>(std::move(updated));
                }

                m_logger->LogInfo(fmt::format("Search directories changed ({} entries); rebuilding the include graph", count));
                shouldRescan = true;
            }
        }

        if (shouldRescan)
        {
            // Which files a directive resolves to depends entirely on these paths/profiles, so every edge in the
            // graph is now suspect.
            RestartWorkspaceScan();
        }
    }

    lsp::SemanticTokens Server::ComputeAndCacheSemanticTokens(const std::string &uriStr, const std::string &text)
    {
        TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

        features::SemanticTokensRequest request{ uriStr, text, tree, m_symbolTable, m_scopeIndex.GetRoot(uriStr) };
        lsp::SemanticTokens tokens = features::GetSemanticTokens(request);
        codec::EncodeSemanticTokens(text, m_positionEncoding, tokens.data);

        // Cached after encoding, so a delta is computed against exactly the bytes the client holds.
        const std::string resultId = std::to_string(++m_semanticTokensRevision);
        tokens.resultId = resultId;
        m_semanticTokensCache[uriStr] = SemanticTokensSnapshot{ resultId, tokens.data };

        return tokens;
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

        m_logger->LogInfo(fmt::format("Workspace folders changed (+{} -{}); now {} root(s), rescanning",
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
                    std::lock_guard<std::mutex> lock(m_predefinedMutex);
                    if (const auto owner = m_predefinedUriByPath.find(path); owner != m_predefinedUriByPath.end())
                    {
                        m_symbolTable.ClearDocumentSymbols(owner->second);
                        m_scopeIndex.ClearDocument(owner->second);
                        m_callGraph.ClearDocument(owner->second);
                        m_predefinedUris.erase(owner->second);
                        m_predefinedUriByPath.erase(owner);
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

            m_includeGraph.UpdateFile(path, content, *SearchDirectories(), IncludeAllowedRoots());
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

        if (!graphChanged)
            return;

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
        bool anyScriptCreated = false;
        for (const auto &created : params.files)
        {
            const std::string path = CanonicalPathFromUri(DocumentKey(created.uri.toString()));
            if (!path.empty() && path.ends_with(m_config.info.fileExtension))
            {
                anyScriptCreated = true;
                break;
            }
        }

        if (!anyScriptCreated)
            return;

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
                directive.rawPath, includerPath, *SearchDirectories(), IncludeAllowedRoots());
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

    void Server::ReanalyseOpenDocuments()
    {
        for (const auto &[openUri, text] : m_openDocuments)
        {
            IndexModuleClosure(openUri);
            ScheduleAnalysis(openUri, text);
        }
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
                m_logger->LogError(fmt::format("Unknown diagnostic severity '{}' for '{}'; ignored", severityName, code));
        }

        if (!m_diagnosticSeverities.empty())
        {
            m_logger->LogInfo(fmt::format("Diagnostic severity overrides active: {}", m_diagnosticSeverities.size()));
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

        if (!changed.empty())
        {
            m_logger->LogInfo(fmt::format("Engine properties differing from the defaults: {}", changed));
        }
    }

    angel_lsp::analysis::SemanticAnalysisRequest Server::BuildAnalysisRequest(const std::string &uriStr,
                                                                              const std::string &text,
                                                                              const TSTree *tree) const
    {
        angel_lsp::analysis::SemanticAnalysisRequest request{
            m_symbolTable, uriStr, std::string(m_config.info.predefinedFileExtension), m_i18n.get()};

        request.typeConfig = &m_config.types;
        request.engineProperties = &m_config.engine;
        request.diagnostics = &m_config.diagnostics;
        request.severityOverrides = m_diagnosticSeverities.empty() ? nullptr : &m_diagnosticSeverities;
        request.enableTypeConversionChecks = m_config.features.enableTypeConversionChecks;
        request.scopeRoot = m_scopeIndex.GetRoot(uriStr);
        request.sourceCode = text;
        request.tree = tree;
        request.excludedLineRanges = ExcludedLineRanges(text);
        return request;
    }

    std::vector<angel_lsp::analysis::Diagnostic> Server::ReplaceSymbolsFromTree(const std::string &uriStr,
                                                                                 const std::string &text,
                                                                                 TSTree *tree)
    {
        angel_lsp::analysis::SymbolTable staging;
        auto diagnostics = m_symbolCollector->CollectSymbolsWithTree(uriStr, text, tree, staging, m_i18n.get(), &m_config.types);
        m_symbolTable.ReplaceDocumentSymbols(uriStr, staging);
        return diagnostics;
    }

    std::vector<angel_lsp::analysis::Diagnostic> Server::ReplaceSymbolsFromSource(const std::string &uriStr,
                                                                                   const std::string &text,
                                                                                   angel_lsp::parser::AngelScriptParser &parser)
    {
        angel_lsp::analysis::SymbolTable staging;
        auto diagnostics = m_symbolCollector->CollectSymbols(uriStr, text, parser, staging, m_i18n.get(), &m_config.types);
        m_symbolTable.ReplaceDocumentSymbols(uriStr, staging);
        return diagnostics;
    }

    std::vector<angel_lsp::analysis::Diagnostic> Server::CollectScopesAndAnalyze(const std::string &uriStr,
                                                                                 const std::string &text,
                                                                                 const TSTree *tree)
    {
        // A language server's input is whatever the user opens, and parts of the analysis are
        // superlinear, so an enormous document is a way to hang the session rather than just slow
        // it. Past the limit the document is still tracked, synced and navigable - it simply is not
        // analysed. Degrading is better than freezing, and better than a size the user cannot see.
        if (text.size() > angel_lsp::constants::limits::MaxAnalysedDocumentBytes)
        {
            m_logger->LogWarning(fmt::format(
                "Skipping analysis of {}: {} bytes exceeds the {} byte limit. Navigation still works.",
                uriStr, text.size(), angel_lsp::constants::limits::MaxAnalysedDocumentBytes));

            m_scopeIndex.ClearDocument(uriStr);
            m_callGraph.ClearDocument(uriStr);
            return {};
        }

        // Held privately until analysis is done - see the header for why the order matters.
        std::shared_ptr<angel_lsp::analysis::Scope> scopeRoot;

        if (tree)
        {
            const TSNode root = ts_tree_root_node(tree);
            scopeRoot = m_localScopeCollector->CollectScopesFromTree(root, text);
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(root, text));
        }
        else
        {
            m_scopeIndex.ClearDocument(uriStr);
            m_callGraph.ClearDocument(uriStr);
        }

        auto request = BuildAnalysisRequest(uriStr, text, tree);

        // Overrides the published snapshot BuildAnalysisRequest looked up. mutableScopeRoot is this
        // function asserting exclusive ownership of that exact tree, which is what makes the
        // `auto` write-back inside the conversion rules safe.
        request.scopeRoot = scopeRoot;
        request.mutableScopeRoot = scopeRoot.get();

        auto diagnostics = m_semanticAnalyzer->Analyze(request);

        if (scopeRoot)
            m_scopeIndex.SetScopeTree(uriStr, std::shared_ptr<const angel_lsp::analysis::Scope>(std::move(scopeRoot)));

        return diagnostics;
    }

    void Server::HandleNotificationsTextDocument_DidSave(lsp::notifications::TextDocument_DidSave::Params &&params)
    {
        std::string uriStr = DocumentKey(params.textDocument.uri.toString());
        // Remembered so diagnostics go back out under the client's own spelling - see
        // m_clientUriByKey. Recorded on every notification that carries a document, because the
        // client is free to change how it writes the URI between them.
        m_clientUriByKey[uriStr] = params.textDocument.uri.toString();
        std::string text = params.text.has_value() ? params.text.value() : "";

        if (text.empty() && m_openDocuments.contains(uriStr))
            text = m_openDocuments[uriStr];

        m_symbolTable.ClearDocumentSymbols(uriStr);
        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            TSTree *savedTree = m_parser->Parse(text);
            std::vector<angel_lsp::analysis::Diagnostic> diagnostics;
            {
                std::lock_guard<std::mutex> lock(m_predefinedMutex);
                ClaimPredefinedFile(uriStr);

                diagnostics = ReplaceSymbolsFromTree(uriStr, text, savedTree);
                m_scopeIndex.ClearDocument(uriStr);
                m_callGraph.ClearDocument(uriStr);
            }

            auto semanticDiagnostics = CollectScopesAndAnalyze(uriStr, text, savedTree);
            diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

            if (savedTree)
                ts_tree_delete(savedTree);

            PublishDiagnostics(uriStr, diagnostics);
            return;
        }

        // Parsed once and shared: symbol collection, scope building and the conversion rules all
        // need the same tree, and letting each of them parse the text again is pure waste.
        TSTree *savedTree = m_parser->Parse(text);

        auto diagnostics = ReplaceSymbolsFromTree(uriStr, text, savedTree);

        // A save is the only point at which an edited #include line can change which module this
        // file belongs to, so the graph is patched here rather than on every keystroke.
        if (const std::string savedPath = CanonicalPathFromUri(uriStr); !savedPath.empty())
            m_includeGraph.UpdateFile(savedPath, text, *SearchDirectories(), IncludeAllowedRoots());

        IndexModuleClosure(uriStr);

        auto semanticDiagnostics = CollectScopesAndAnalyze(uriStr, text, savedTree);
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

        if (savedTree)
            ts_tree_delete(savedTree);

        AppendIncludeDiagnostics(uriStr, text, diagnostics);

        PublishDiagnostics(uriStr, diagnostics);
    }

    void Server::HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params &&params)
    {
        std::string uriStr = DocumentKey(params.textDocument.uri.toString());
        // Remembered so diagnostics go back out under the client's own spelling - see
        // m_clientUriByKey. Recorded on every notification that carries a document, because the
        // client is free to change how it writes the URI between them.
        m_clientUriByKey[uriStr] = params.textDocument.uri.toString();
        std::string text = params.textDocument.text;

        m_openDocuments[uriStr] = text;

        if (auto treeIt = m_documentTrees.find(uriStr); treeIt != m_documentTrees.end())
        {
            if (treeIt->second)
                ts_tree_delete(treeIt->second);
        }
        TSTree *tree = m_parser->Parse(text);
        m_documentTrees[uriStr] = tree;

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            std::vector<angel_lsp::analysis::Diagnostic> diagnostics;
            {
                std::lock_guard<std::mutex> lock(m_predefinedMutex);
                if (ClaimPredefinedFile(uriStr))
                {
                    diagnostics = ReplaceSymbolsFromTree(uriStr, text, tree);
                    m_scopeIndex.ClearDocument(uriStr);
                    m_callGraph.ClearDocument(uriStr);
                }
            }

            auto semanticDiagnostics = CollectScopesAndAnalyze(uriStr, text, tree);
            diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

            PublishDiagnostics(uriStr, diagnostics);
            return;
        }

        auto diagnostics = ReplaceSymbolsFromTree(uriStr, text, tree);

        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);

        // Before analysis, not after: the module the file belongs to supplies declarations this
        // file legitimately uses, and without them every one of them would be reported undeclared.
        IndexModuleClosure(uriStr);

        auto semanticDiagnostics = CollectScopesAndAnalyze(uriStr, text, tree);
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

        AppendIncludeDiagnostics(uriStr, text, diagnostics);

        PublishDiagnostics(uriStr, diagnostics);
    }

    void Server::HandleNotificationsTextDocument_DidChange(lsp::notifications::TextDocument_DidChange::Params &&params)
    {
        std::string uriStr = DocumentKey(params.textDocument.uri.toString());
        // Remembered so diagnostics go back out under the client's own spelling - see
        // m_clientUriByKey. Recorded on every notification that carries a document, because the
        // client is free to change how it writes the URI between them.
        m_clientUriByKey[uriStr] = params.textDocument.uri.toString();
        auto it = m_openDocuments.find(uriStr);
        if (it == m_openDocuments.end())
            return;

        std::string &buffer = it->second;

        auto treeIt = m_documentTrees.find(uriStr);
        TSTree *tree = (treeIt != m_documentTrees.end()) ? treeIt->second : nullptr;

        for (const auto &change : params.contentChanges)
        {
            if (std::holds_alternative<lsp::TextDocumentContentChangePartial>(change))
            {
                const auto &rt = std::get<lsp::TextDocumentContentChangePartial>(change);

                if (tree)
                {
                    uint32_t startLine = rt.range.start.line;
                    // rt.range is expressed in the negotiated encoding, while TSPoint::column and every
                    // byte offset below are byte columns. Converting here is what keeps the server's
                    // buffer in step with the editor on documents containing non-ASCII text: an edit
                    // applied at the wrong offset desynchronises the two permanently.
                    uint32_t startChar = angel_lsp::utils::LspCharToByteColumn(
                        angel_lsp::utils::GetLine(buffer, startLine), rt.range.start.character, m_positionEncoding);
                    uint32_t endLine = rt.range.end.line;
                    uint32_t endChar = angel_lsp::utils::LspCharToByteColumn(
                        angel_lsp::utils::GetLine(buffer, endLine), rt.range.end.character, m_positionEncoding);

                    uint32_t start_byte = static_cast<uint32_t>(angel_lsp::utils::PositionToOffset(buffer, startLine, rt.range.start.character, m_positionEncoding));
                    uint32_t old_end_byte = static_cast<uint32_t>(angel_lsp::utils::PositionToOffset(buffer, endLine, rt.range.end.character, m_positionEncoding));
                    uint32_t new_end_byte = static_cast<uint32_t>(start_byte + rt.text.size());

                    TSPoint start_point = { startLine, startChar };
                    TSPoint old_end_point = { endLine, endChar };

                    uint32_t newlineCount = 0;
                    size_t lastNewlinePos = std::string::npos;
                    for (size_t i = 0; i < rt.text.size(); ++i)
                    {
                        if (rt.text[i] == '\n')
                        {
                            newlineCount++;
                            lastNewlinePos = i;
                        }
                    }

                    TSPoint new_end_point;
                    if (newlineCount > 0)
                    {
                        new_end_point.row = startLine + newlineCount;
                        new_end_point.column = static_cast<uint32_t>(rt.text.size() - (lastNewlinePos + 1));
                    }
                    else
                    {
                        new_end_point.row = startLine;
                        new_end_point.column = static_cast<uint32_t>(startChar + rt.text.size());
                    }

                    TSInputEdit edit;
                    edit.start_byte = start_byte;
                    edit.old_end_byte = old_end_byte;
                    edit.new_end_byte = new_end_byte;
                    edit.start_point = start_point;
                    edit.old_end_point = old_end_point;
                    edit.new_end_point = new_end_point;

                    ts_tree_edit(tree, &edit);
                }

                angel_lsp::utils::ApplyIncrementalChange(buffer,
                                                         rt.range.start.line, rt.range.start.character,
                                                         rt.range.end.line, rt.range.end.character,
                                                         rt.text, m_positionEncoding);
            }
            else if (std::holds_alternative<lsp::TextDocumentContentChangeWholeDocument>(change))
            {
                const auto &t = std::get<lsp::TextDocumentContentChangeWholeDocument>(change);
                buffer = t.text;
                if (tree)
                {
                    ts_tree_delete(tree);
                    tree = nullptr;
                    m_documentTrees.erase(uriStr);
                }
            }
        }

        TSTree *oldTree = tree;
        TSTree *newTree = m_parser->Parse(buffer, oldTree);
        if (oldTree)
        {
            ts_tree_delete(oldTree);
        }
        m_documentTrees[uriStr] = newTree;

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            m_symbolTable.ClearDocumentSymbols(uriStr);
            m_scopeIndex.ClearDocument(uriStr);
            m_callGraph.ClearDocument(uriStr);
            ReplaceSymbolsFromTree(uriStr, buffer, newTree);
            if (newTree)
            {
                m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(newTree), buffer));
                m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(newTree), buffer));
            }
            ScheduleAnalysis(uriStr, buffer);
            return;
        }

        // The reparse above is incremental and cheap, and stays on this thread so a request
        // arriving right after the edit is answered against a current tree. Symbol collection,
        // scope building and semantic analysis rebuild whole-document state instead - that is what
        // makes a 3000-line file feel slow when it runs on every keystroke - so they are queued and
        // run once typing pauses. Until then the symbol table still holds the previous revision,
        // which is the same trade every other language server makes.
        ScheduleAnalysis(uriStr, buffer);
    }

    void Server::HandleNotificationsTextDocument_DidClose(lsp::notifications::TextDocument_DidClose::Params &&params)
    {
        std::string uriStr = DocumentKey(params.textDocument.uri.toString());
        m_openDocuments.erase(uriStr);

        // The cached token payload is only meaningful while the client still holds it. Dropping it
        // here also means a reopened document starts from a full stream rather than a delta against
        // a payload the client threw away when it closed the editor tab.
        m_semanticTokensCache.erase(uriStr);

        auto it = m_documentTrees.find(uriStr);
        if (it != m_documentTrees.end())
        {
            if (it->second)
            {
                ts_tree_delete(it->second);
            }
            m_documentTrees.erase(it);
        }

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            PublishDiagnostics(uriStr, {});
            return;
        }

        ReleaseModuleClosure(uriStr);

        m_symbolTable.ClearDocumentSymbols(uriStr);
        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);

        if (const std::string path = CanonicalPathFromUri(uriStr); !path.empty())
        {
            if (const auto indexed = m_indexedUriByPath.find(path); indexed != m_indexedUriByPath.end() && indexed->second == uriStr)
                m_indexedUriByPath.erase(indexed);
        }

        // Closing a file does not remove it from the modules of the documents still open. Re-running
        // their closures picks it back up as an on-disk closure file - and costs nothing for the
        // files already indexed, which are skipped by URI.
        std::vector<std::string> stillOpen;
        stillOpen.reserve(m_openDocuments.size());
        for (const auto &[openUri, _] : m_openDocuments)
            stillOpen.push_back(openUri);

        for (const auto &openUri : stillOpen)
        {
            if (!angel_lsp::utils::IsPredefinedFile(openUri, m_config.info.predefinedFileExtension))
                IndexModuleClosure(openUri);
        }

        PublishDiagnostics(uriStr, {});
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

    void Server::IndexClosureFile(const std::string &path, angel_lsp::parser::AngelScriptParser &parser)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return;

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (content.empty())
            return;

        const std::string uriStr = UriFromPath(path);

        TSTree *tree = parser.Parse(content);

        ReplaceSymbolsFromTree(uriStr, content, tree);

        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);
        if (tree)
        {
            m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(tree), content));
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(tree), content));
        }

        // The tree is only needed to collect symbols; keeping one per closure file would multiply
        // memory across a large module for no benefit, since no request is ever served from a file
        // the user has not opened.
        if (tree)
            ts_tree_delete(tree);

        m_closureDocuments[uriStr] = std::move(content);
        m_indexedUriByPath[path] = uriStr;
    }

    namespace
    {
        /**
         * @brief How long editing has to pause before a document is re-analysed.
         *
         * Long enough that a burst of typing collapses into one run, short enough that diagnostics
         * still feel attached to the edit that caused them.
         */
        constexpr std::chrono::milliseconds k_analysisDebounce{200};
    }

    void Server::ScheduleAnalysis(const std::string &uriStr, const std::string &text)
    {
        {
            std::lock_guard<std::mutex> lock(m_analysisMutex);
            m_pendingAnalysis[uriStr] = text;
            ++m_analysisRevision;
        }

        m_analysisCv.notify_one();
    }

    void Server::RunAnalysisLoop()
    {
        // One parser for the life of the thread. Private to it, so no synchronisation is needed and
        // none of the message loop's trees are ever touched from here.
        angel_lsp::parser::AngelScriptParser parser(m_logger.get());

        for (;;)
        {
            std::unique_lock<std::mutex> lock(m_analysisMutex);
            m_analysisCv.wait(lock, [this]() { return m_analysisStop || !m_pendingAnalysis.empty(); });

            if (m_analysisStop)
                return;

            // Hold off while edits keep arriving: each new one bumps the revision and restarts the
            // quiet period, so a burst of keystrokes costs exactly one analysis instead of one per
            // character.
            for (;;)
            {
                const uint64_t seen = m_analysisRevision;
                const bool interrupted = m_analysisCv.wait_for(lock, k_analysisDebounce, [this, seen]()
                                                               { return m_analysisStop || m_analysisRevision != seen; });

                if (m_analysisStop)
                    return;

                if (!interrupted)
                    break;
            }

            ankerl::unordered_dense::map<std::string, std::string> batch;
            batch.swap(m_pendingAnalysis);
            lock.unlock();

            for (const auto &[uriStr, text] : batch)
                AnalyzeDocument(uriStr, text, parser);
        }
    }

    void Server::AnalyzeDocument(const std::string &uriStr, const std::string &text,
                                 angel_lsp::parser::AngelScriptParser &parser)
    {
        // Own tree, own copy of the text. The message loop owns m_documentTrees and deletes the
        // tree there on the next edit; reading it from here would be a use-after-free.
        //
        // The parser is handed in and reused across the batch rather than constructed per document:
        // each construction is a ts_parser_new plus a language load, paid once per document per
        // debounced analysis for nothing. It stays private to the analysis thread, which is what
        // keeps that safe - a TSParser is not shareable.
        TSTree *tree = parser.Parse(text);

        // Collected into a staging table and swapped in one step, so a reader on the message loop
        // never catches this document mid-rebuild with no symbols at all.
        angel_lsp::analysis::SymbolTable staging;
        auto diagnostics = m_symbolCollector->CollectSymbolsWithTree(uriStr, text, tree, staging, m_i18n.get(), &m_config.types);
        m_symbolTable.ReplaceDocumentSymbols(uriStr, staging);

        // Analysed before the tree below is deleted, not after: the conversion rules read
        // expressions straight out of it, and it is the only tree this thread is allowed to touch.
        auto semanticDiagnostics = CollectScopesAndAnalyze(uriStr, text, tree);
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

        if (tree)
            ts_tree_delete(tree);

        AppendIncludeDiagnostics(uriStr, text, diagnostics);

        PublishDiagnostics(uriStr, text, diagnostics);
    }

    void Server::AppendIncludeDiagnostics(const std::string &uriStr, const std::string &text, std::vector<angel_lsp::analysis::Diagnostic> &diagnostics) const
    {
        if (!m_config.features.enableDocumentLink)
            return;

        // Takes the text rather than looking it up: this also runs on the analysis thread, where
        // reading m_openDocuments would race the message loop.
        // Named, not inlined into the initialiser: DocumentLinkRequest holds the vector by
        // reference, so the handle has to outlive the request rather than die at the semicolon.
        const auto searchDirectories = SearchDirectories();

        features::DocumentLinkRequest request{ uriStr, text, *searchDirectories, m_i18n.get(), IncludeAllowedRoots() };
        auto includeDiagnostics = features::GetUnresolvedIncludeDiagnostics(request);
        diagnostics.insert(diagnostics.end(), includeDiagnostics.begin(), includeDiagnostics.end());
    }

    void Server::PurgeClosureFile(const std::string &uriStr)
    {
        // Or workspace/diagnostic would keep reporting a file that is gone, under a result id no
        // edit can ever invalidate.
        {
            std::lock_guard<std::mutex> lock(m_diagnosticsCacheMutex);
            m_diagnosticsCache.erase(uriStr);
        }

        m_symbolTable.ClearDocumentSymbols(uriStr);
        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);
        m_closureDocuments.erase(uriStr);

        const std::string path = CanonicalPathFromUri(uriStr);
        if (!path.empty())
        {
            if (const auto it = m_indexedUriByPath.find(path); it != m_indexedUriByPath.end() && it->second == uriStr)
                m_indexedUriByPath.erase(it);
        }
    }

    void Server::IndexModuleClosure(const std::string &openUriStr)
    {
        const std::string openPath = CanonicalPathFromUri(openUriStr);
        if (openPath.empty())
            return;

        // The open document owns its own URI from here on. If it was previously indexed as a
        // closure file of some other document, that entry is under a synthesised URI spelling and
        // has to go, or every symbol in the file would exist twice.
        if (const auto previous = m_indexedUriByPath.find(openPath); previous != m_indexedUriByPath.end())
        {
            if (previous->second != openUriStr)
                PurgeClosureFile(previous->second);
        }
        m_indexedUriByPath[openPath] = openUriStr;

        std::vector<std::string> indexed;
        angel_lsp::parser::AngelScriptParser closureParser(m_logger.get());

        for (const auto &path : m_includeGraph.GetModuleClosure(openPath))
        {
            if (path == openPath)
                continue;

            const std::string uriStr = UriFromPath(path);

            // Never shadow a document the user has open: its buffer is authoritative and may hold
            // unsaved edits the copy on disk does not.
            if (m_openDocuments.contains(uriStr))
                continue;

            if (const auto already = m_indexedUriByPath.find(path);
                already != m_indexedUriByPath.end() && m_openDocuments.contains(already->second))
            {
                continue;
            }

            if (!m_closureDocuments.contains(uriStr))
                IndexClosureFile(path, closureParser);

            indexed.push_back(uriStr);
        }

        if (!indexed.empty())
        {
            m_logger->LogInfo(fmt::format("Indexed {} file(s) from the #include module of {}", indexed.size(), openUriStr));
        }

        m_openDocumentClosures[openUriStr] = std::move(indexed);
    }

    void Server::ReleaseModuleClosure(const std::string &openUriStr)
    {
        const auto closure = m_openDocumentClosures.find(openUriStr);
        if (closure == m_openDocumentClosures.end())
            return;

        const std::vector<std::string> released = std::move(closure->second);
        m_openDocumentClosures.erase(closure);

        for (const auto &uriStr : released)
        {
            // A closure file shared by two open documents outlives the first one to close.
            bool stillNeeded = false;
            for (const auto &[otherUri, otherClosure] : m_openDocumentClosures)
            {
                if (std::find(otherClosure.begin(), otherClosure.end(), uriStr) != otherClosure.end())
                {
                    stillNeeded = true;
                    break;
                }
            }

            if (!stillNeeded)
                PurgeClosureFile(uriStr);
        }
    }

    const std::string *Server::FindDocumentText(const std::string &uri) const
    {
        if (const auto open = m_openDocuments.find(uri); open != m_openDocuments.end())
            return &open->second;

        // Closure files are not open, but their ranges still reach the client through references,
        // definitions and multi-file rename edits, so their text has to be reachable too.
        if (const auto closure = m_closureDocuments.find(uri); closure != m_closureDocuments.end())
            return &closure->second;

        return nullptr;
    }

    void Server::EncodeIn(std::string_view text, lsp::Range &range) const
    {
        codec::Encode(text, m_positionEncoding, range);
    }

    void Server::EncodeIn(std::string_view text, lsp::Hover &hover) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8 || !hover.range.has_value())
            return;

        codec::Encode(text, m_positionEncoding, hover.range.value());
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::TextEdit> &edits) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &edit : edits)
            codec::Encode(text, m_positionEncoding, edit.range);
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentHighlight> &highlights) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &highlight : highlights)
            codec::Encode(text, m_positionEncoding, highlight.range);
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::FoldingRange> &ranges) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        // FoldingRange carries loose columns rather than a Range, and both are optional: a
        // line-granularity fold omits them entirely and needs no conversion.
        for (auto &range : ranges)
        {
            if (range.startCharacter.has_value())
            {
                range.startCharacter = angel_lsp::utils::ByteToLspCharColumn(
                    angel_lsp::utils::GetLine(text, range.startLine), range.startCharacter.value(), m_positionEncoding);
            }

            if (range.endCharacter.has_value())
            {
                range.endCharacter = angel_lsp::utils::ByteToLspCharColumn(
                    angel_lsp::utils::GetLine(text, range.endLine), range.endCharacter.value(), m_positionEncoding);
            }
        }
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentLink> &links) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &link : links)
            codec::Encode(text, m_positionEncoding, link.range);
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::InlayHint> &hints) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &hint : hints)
        {
            codec::Encode(text, m_positionEncoding, hint.position);

            if (hint.textEdits.has_value())
            {
                for (auto &edit : hint.textEdits.value())
                    codec::Encode(text, m_positionEncoding, edit.range);
            }
        }
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::DocumentSymbol> &symbols) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &symbol : symbols)
        {
            codec::Encode(text, m_positionEncoding, symbol.range);
            codec::Encode(text, m_positionEncoding, symbol.selectionRange);

            if (symbol.children.has_value())
                EncodeIn(text, symbol.children.value());
        }
    }

    void Server::EncodeIn(std::string_view text, std::vector<lsp::CodeLens> &lenses) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &lens : lenses)
            codec::Encode(text, m_positionEncoding, lens.range);
    }

    void Server::EncodeIn(std::string_view text, lsp::PrepareRenameResult &result) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        if (auto *range = std::get_if<lsp::Range>(&result))
            codec::Encode(text, m_positionEncoding, *range);
        else if (auto *placeholder = std::get_if<lsp::PrepareRenamePlaceholder>(&result))
            codec::Encode(text, m_positionEncoding, placeholder->range);
    }

    void Server::EncodeAcrossDocuments(std::vector<lsp::Location> &locations) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &location : locations)
        {
            if (const std::string *text = FindDocumentText(DocumentKey(location.uri.toString())))
                codec::Encode(*text, m_positionEncoding, location.range);
        }
    }

    void Server::EncodeAcrossDocuments(std::vector<lsp::SymbolInformation> &symbols) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &symbol : symbols)
        {
            if (const std::string *text = FindDocumentText(DocumentKey(symbol.location.uri.toString())))
                codec::Encode(*text, m_positionEncoding, symbol.location.range);
        }
    }

    void Server::EncodeAcrossDocuments(lsp::WorkspaceEdit &edit) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        if (edit.changes.has_value())
        {
            for (auto &[uri, edits] : edit.changes.value())
            {
                if (const std::string *text = FindDocumentText(uri.toString()))
                    EncodeIn(*text, edits);
            }
        }

        if (edit.documentChanges.has_value())
        {
            for (auto &docChange : edit.documentChanges.value())
            {
                if (std::holds_alternative<lsp::TextDocumentEdit>(docChange))
                {
                    auto &docEdit = std::get<lsp::TextDocumentEdit>(docChange);
                    if (const std::string *text = FindDocumentText(DocumentKey(docEdit.textDocument.uri.toString())))
                    {
                        for (auto &e : docEdit.edits)
                        {
                            if (std::holds_alternative<lsp::TextEdit>(e))
                            {
                                auto &te = std::get<lsp::TextEdit>(e);
                                codec::Encode(*text, m_positionEncoding, te.range);
                            }
                            else if (std::holds_alternative<lsp::AnnotatedTextEdit>(e))
                            {
                                auto &ate = std::get<lsp::AnnotatedTextEdit>(e);
                                codec::Encode(*text, m_positionEncoding, ate.range);
                            }
                            else if (std::holds_alternative<lsp::SnippetTextEdit>(e))
                            {
                                auto &ste = std::get<lsp::SnippetTextEdit>(e);
                                codec::Encode(*text, m_positionEncoding, ste.range);
                            }
                        }
                    }
                }
            }
        }
    }

    void Server::EncodeRangesIn(const std::string &uri, std::vector<lsp::Range> &ranges) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        if (const std::string *text = FindDocumentText(uri))
        {
            for (auto &range : ranges)
                codec::Encode(*text, m_positionEncoding, range);
        }
    }

    void Server::EncodeAcrossDocuments(std::vector<lsp::CodeAction> &actions) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &action : actions)
        {
            if (action.edit.has_value())
                EncodeAcrossDocuments(action.edit.value());
        }
    }

    void Server::PublishDiagnostics(const std::string &uriStr, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics)
    {
        const std::string *docText = FindDocumentText(uriStr);
        PublishDiagnostics(uriStr, docText ? *docText : std::string(), diagnostics);
    }

    std::vector<lsp::Diagnostic> Server::ToProtocolDiagnostics(const std::string &text, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics) const
    {
        // Applied here, at the one place every diagnostic passes through on its way to the client.
        // SemanticAnalyzer already filters its own output, but symbol-collection and unresolved-
        // include diagnostics do not go through it, and a `#if` block that the preprocessor removes
        // is not code no matter which pass produced the complaint about it.
        const auto excluded = ExcludedLineRanges(text);

        std::vector<lsp::Diagnostic> converted;
        converted.reserve(diagnostics.size());

        for (const auto &diag : diagnostics)
        {
            if (!excluded.empty() && angel_lsp::utils::IsLineExcluded(excluded, diag.range.start.line))
                continue;

            lsp::Diagnostic lspDiag;
            lspDiag.range.start.line = diag.range.start.line;
            lspDiag.range.start.character = diag.range.start.character;
            lspDiag.range.end.line = diag.range.end.line;
            lspDiag.range.end.character = diag.range.end.character;
            lspDiag.message = diag.message;
            lspDiag.severity = ToProtocolSeverity(diag.severity);
            lspDiag.source = diag.source;
            lspDiag.code = diag.code;

            // An empty text means the server holds no copy of the document; leaving the byte
            // columns alone beats converting them against nothing, which would collapse every
            // range to column zero.
            if (!text.empty())
                codec::Encode(text, m_positionEncoding, lspDiag.range);

            converted.push_back(std::move(lspDiag));
        }

        return converted;
    }

    void Server::PublishDiagnostics(const std::string &uriStr, const std::string &text, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics)
    {
        lsp::notifications::TextDocument_PublishDiagnostics::Params params;

        // Back out under the CLIENT's spelling, not the internal key. Every map in this server is
        // keyed by DocumentKey so that one file cannot become several documents, and that key is
        // `file:///E:/dir/f.as` where VS Code sends `file:///e%3A/dir/f.as`. To a client matching a
        // notification against its open editors those are two different documents, so publishing
        // under the key would have keyed everything correctly and shown the user nothing.
        //
        // Falls back to the key for a document the client never announced - the workspace scan
        // indexes files nobody has opened, and a synthesised URI is the only spelling there is.
        const auto clientUri = m_clientUriByKey.find(uriStr);
        const std::string &outgoingUri = clientUri != m_clientUriByKey.end() ? clientUri->second : uriStr;
        params.uri = lsp::DocumentUri(lsp::Uri::parse(outgoingUri));

        params.diagnostics = ToProtocolDiagnostics(text, diagnostics);

        // Cached for the pull path before it goes out, under a fresh result id. The client echoes
        // that id back and is told `unchanged` while it still matches, so a file nobody edits costs
        // one string per poll instead of its whole diagnostic list.
        {
            std::lock_guard<std::mutex> cacheLock(m_diagnosticsCacheMutex);
            DiagnosticsSnapshot snapshot;
            snapshot.resultId = std::to_string(++m_diagnosticsRevision);
            snapshot.items = params.diagnostics;
            m_diagnosticsCache[uriStr] = std::move(snapshot);
        }

        std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
        m_messageHandler->sendNotification<lsp::notifications::TextDocument_PublishDiagnostics>(std::move(params));
    }

    lsp::requests::TextDocument_Diagnostic::Result Server::HandleRequestsTextDocument_Diagnostic(lsp::requests::TextDocument_Diagnostic::Params &&params)
    {
        if (!m_config.features.enablePullDiagnostics)
        {
            throw lsp::RequestError(lsp::MessageError::MethodNotFound, "Pull diagnostics are disabled");
        }

        const std::string uriStr = DocumentKey(params.textDocument.uri.toString());

        {
            std::lock_guard<std::mutex> lock(m_diagnosticsCacheMutex);
            if (const auto it = m_diagnosticsCache.find(uriStr); it != m_diagnosticsCache.end())
            {
                if (params.previousResultId.has_value() && *params.previousResultId == it->second.resultId)
                {
                    lsp::RelatedUnchangedDocumentDiagnosticReport unchanged;
                    unchanged.resultId = it->second.resultId;
                    return unchanged;
                }

                lsp::RelatedFullDocumentDiagnosticReport full;
                full.resultId = it->second.resultId;
                full.items = it->second.items;
                return full;
            }
        }

        // Nothing analysed yet. Queue it and tell the client to ask again rather than answering
        // with an empty report - an empty report says "this file is clean", which is a claim this
        // server is in no position to make about a document it has not looked at.
        if (const std::string *text = FindDocumentText(uriStr))
            ScheduleAnalysis(uriStr, *text);

        lsp::json::Object retrigger;
        retrigger["retriggerRequest"] = true;

        throw lsp::RequestError(lsp::MessageError::ServerCancelled,
                                "Diagnostics for this document are still being computed",
                                lsp::json::Value(std::move(retrigger)));
    }

    lsp::requests::Workspace_Diagnostic::Result Server::HandleRequestsWorkspace_Diagnostic(lsp::requests::Workspace_Diagnostic::Params &&params)
    {
        if (!m_config.features.enablePullDiagnostics)
        {
            return lsp::WorkspaceDiagnosticReport{};
        }

        // What the client already holds, so an unedited document can be answered with its id alone.
        ankerl::unordered_dense::map<std::string, std::string> known;
        for (const auto &previous : params.previousResultIds)
            known[DocumentKey(previous.uri.toString())] = previous.value;

        lsp::WorkspaceDiagnosticReport report;

        std::lock_guard<std::mutex> lock(m_diagnosticsCacheMutex);
        for (const auto &[uriStr, snapshot] : m_diagnosticsCache)
        {
            // Out under the client's own spelling, for the same reason PublishDiagnostics does it:
            // the key is canonical and the client matches these against its own URIs.
            const auto clientUri = m_clientUriByKey.find(uriStr);
            const std::string &outgoingUri = clientUri != m_clientUriByKey.end() ? clientUri->second : uriStr;

            if (const auto it = known.find(uriStr); it != known.end() && it->second == snapshot.resultId)
            {
                lsp::WorkspaceUnchangedDocumentDiagnosticReport unchanged;
                unchanged.uri = lsp::DocumentUri(lsp::Uri::parse(outgoingUri));
                unchanged.version = nullptr;
                unchanged.resultId = snapshot.resultId;
                report.items.push_back(std::move(unchanged));
                continue;
            }

            lsp::WorkspaceFullDocumentDiagnosticReport full;
            full.uri = lsp::DocumentUri(lsp::Uri::parse(outgoingUri));
            full.version = nullptr;
            full.resultId = snapshot.resultId;
            full.items = snapshot.items;
            report.items.push_back(std::move(full));
        }

        return report;
    }

    void Server::InitHandles()
    {
        m_messageHandler->add<lsp::requests::Initialize>(
            [this](lsp::requests::Initialize::Params &&params)
            {
                return this->HandleRequestsInitialized(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Initialized>(
            [this](lsp::notifications::Initialized::Params &&params)
            {
                this->HandleNotificationsInitialized(std::move(params));
            });

        m_messageHandler->add<lsp::requests::Shutdown>(
            [this]()
            {
                return this->HandleRequestsShutdown();
            });

        m_messageHandler->add<lsp::notifications::Window_WorkDoneProgress_Cancel>(
            [this](lsp::notifications::Window_WorkDoneProgress_Cancel::Params &&params)
            {
                this->HandleNotificationsWindow_WorkDoneProgress_Cancel(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::SetTrace>(
            [this](lsp::notifications::SetTrace::Params &&params)
            {
                this->HandleNotificationsSetTrace(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidCreateFiles>(
            [this](lsp::notifications::Workspace_DidCreateFiles::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidCreateFiles(std::move(params));
            });

        m_messageHandler->add<lsp::requests::TextDocument_Diagnostic>(
            [this](lsp::requests::TextDocument_Diagnostic::Params &&params)
            {
                return this->HandleRequestsTextDocument_Diagnostic(std::move(params));
            });

        m_messageHandler->add<lsp::requests::Workspace_Diagnostic>(
            [this](lsp::requests::Workspace_Diagnostic::Params &&params)
            {
                return this->HandleRequestsWorkspace_Diagnostic(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Exit>(
            [this]()
            {
                this->HandleNotificationsExit();
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidChangeConfiguration>(
            [this](lsp::notifications::Workspace_DidChangeConfiguration::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidChangeConfiguration(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidRenameFiles>(
            [this](lsp::notifications::Workspace_DidRenameFiles::Params &&params)
            {
                HandleNotificationsWorkspace_DidRenameFiles(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidDeleteFiles>(
            [this](lsp::notifications::Workspace_DidDeleteFiles::Params &&params)
            {
                HandleNotificationsWorkspace_DidDeleteFiles(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidChangeWatchedFiles>(
            [this](lsp::notifications::Workspace_DidChangeWatchedFiles::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidChangeWatchedFiles(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidChangeWorkspaceFolders>(
            [this](lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidChangeWorkspaceFolders(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidSave>(
            [this](lsp::notifications::TextDocument_DidSave::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidSave(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidOpen>(
            [this](lsp::notifications::TextDocument_DidOpen::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidOpen(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidChange>(
            [this](lsp::notifications::TextDocument_DidChange::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidChange(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidClose>(
            [this](lsp::notifications::TextDocument_DidClose::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidClose(std::move(params));
            });

        m_messageHandler->add<lsp::requests::TextDocument_Hover>(
            [this](lsp::requests::TextDocument_Hover::Params &&req) -> lsp::requests::TextDocument_Hover::Result
            {
                if (!m_config.features.enableHover)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::HoverRequest hr{
                    uriStr,
                    docIt->second,
                    tree,
                    m_symbolTable,
                    m_scopeIndex,
                    codec::Decode(docIt->second, m_positionEncoding, req.position),
                    // A symbol's documentation comment lives above its declaration, which is
                    // usually in another file. The same reader CompletionItem/resolve is given.
                    [this](const std::string &uri) { return FindDocumentText(uri); }
                };
                auto hover = features::GetHover(hr);
                if (hover.has_value())
                {
                    EncodeIn(docIt->second, hover.value());
                    return hover.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Definition>(
            [this](lsp::requests::TextDocument_Definition::Params &&req) -> lsp::requests::TextDocument_Definition::Result
            {
                if (!m_config.features.enableDefinition)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::DefinitionRequest dr{ uriStr, docIt->second, tree, m_symbolTable, m_scopeIndex, codec::Decode(docIt->second, m_positionEncoding, req.position) };
                auto defs = features::GetDefinition(dr);
                if (defs.has_value() && !defs->empty())
                {
                    EncodeAcrossDocuments(defs.value());
                    return defs.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Declaration>(
            [this](lsp::requests::TextDocument_Declaration::Params &&req) -> lsp::requests::TextDocument_Declaration::Result
            {
                // Deliberately the same handler as textDocument/definition: see the capability.
                if (!m_config.features.enableDefinition)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::DefinitionRequest dr{ uriStr, docIt->second, tree, m_symbolTable, m_scopeIndex, codec::Decode(docIt->second, m_positionEncoding, req.position) };
                auto defs = features::GetDefinition(dr);
                if (defs.has_value() && !defs->empty())
                {
                    EncodeAcrossDocuments(defs.value());
                    return defs.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Implementation>(
            [this](lsp::requests::TextDocument_Implementation::Params &&req) -> lsp::requests::TextDocument_Implementation::Result
            {
                if (!m_config.features.enableImplementation)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::ImplementationRequest ir{ uriStr, docIt->second, tree, m_symbolTable, codec::Decode(docIt->second, m_positionEncoding, req.position) };
                auto impls = features::GetImplementations(ir);
                if (impls.has_value() && !impls->empty())
                {
                    EncodeAcrossDocuments(impls.value());
                    return impls.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_PrepareCallHierarchy>(
            [this](lsp::requests::TextDocument_PrepareCallHierarchy::Params &&req) -> lsp::requests::TextDocument_PrepareCallHierarchy::Result
            {
                if (!m_config.features.enableCallHierarchy)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::CallHierarchyPrepareRequest pr{ uriStr, docIt->second, tree, m_symbolTable, codec::Decode(docIt->second, m_positionEncoding, req.position) };
                auto items = features::PrepareCallHierarchy(pr);
                if (!items.has_value() || items->empty())
                {
                    return lsp::Null{};
                }
                for (auto &item : items.value())
                {
                    EncodeItemRanges(item);
                }
                return items.value();
            });

        m_messageHandler->add<lsp::requests::CallHierarchy_IncomingCalls>(
            [this](lsp::requests::CallHierarchy_IncomingCalls::Params &&req) -> lsp::requests::CallHierarchy_IncomingCalls::Result
            {
                if (!m_config.features.enableCallHierarchy)
                {
                    return lsp::Null{};
                }

                features::CallHierarchyItemRequest ir{ m_symbolTable, m_callGraph, req.item };
                auto calls = features::GetIncomingCalls(ir);
                if (!calls.has_value() || calls->empty())
                {
                    return lsp::Null{};
                }
                for (auto &call : calls.value())
                {
                    EncodeItemRanges(call.from);
                    EncodeRangesIn(DocumentKey(call.from.uri.toString()), call.fromRanges);
                }
                return calls.value();
            });

        m_messageHandler->add<lsp::requests::CallHierarchy_OutgoingCalls>(
            [this](lsp::requests::CallHierarchy_OutgoingCalls::Params &&req) -> lsp::requests::CallHierarchy_OutgoingCalls::Result
            {
                if (!m_config.features.enableCallHierarchy)
                {
                    return lsp::Null{};
                }

                // fromRanges are ranges in the *caller*, which is the item the client asked about -
                // not in the callee the entry points at. Encoding them against the callee's
                // document would shift every one of them on a file with non-ASCII text.
                const std::string callerUri = DocumentKey(req.item.uri.toString());

                features::CallHierarchyItemRequest ir{ m_symbolTable, m_callGraph, req.item };
                auto calls = features::GetOutgoingCalls(ir);
                if (!calls.has_value() || calls->empty())
                {
                    return lsp::Null{};
                }
                for (auto &call : calls.value())
                {
                    EncodeItemRanges(call.to);
                    EncodeRangesIn(callerUri, call.fromRanges);
                }
                return calls.value();
            });

        m_messageHandler->add<lsp::requests::TextDocument_PrepareTypeHierarchy>(
            [this](lsp::requests::TextDocument_PrepareTypeHierarchy::Params &&req) -> lsp::requests::TextDocument_PrepareTypeHierarchy::Result
            {
                if (!m_config.features.enableTypeHierarchy)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::TypeHierarchyPrepareRequest pr{ uriStr, docIt->second, tree, m_symbolTable, codec::Decode(docIt->second, m_positionEncoding, req.position) };
                auto items = features::PrepareTypeHierarchy(pr);
                if (!items.has_value() || items->empty())
                {
                    return lsp::Null{};
                }
                for (auto &item : items.value())
                {
                    EncodeItemRanges(item);
                }
                return items.value();
            });

        m_messageHandler->add<lsp::requests::TypeHierarchy_Supertypes>(
            [this](lsp::requests::TypeHierarchy_Supertypes::Params &&req) -> lsp::requests::TypeHierarchy_Supertypes::Result
            {
                if (!m_config.features.enableTypeHierarchy)
                {
                    return lsp::Null{};
                }

                features::TypeHierarchyItemRequest ir{ m_symbolTable, req.item };
                auto items = features::GetSupertypes(ir);
                if (!items.has_value() || items->empty())
                {
                    return lsp::Null{};
                }
                for (auto &item : items.value())
                {
                    EncodeItemRanges(item);
                }
                return items.value();
            });

        m_messageHandler->add<lsp::requests::TypeHierarchy_Subtypes>(
            [this](lsp::requests::TypeHierarchy_Subtypes::Params &&req) -> lsp::requests::TypeHierarchy_Subtypes::Result
            {
                if (!m_config.features.enableTypeHierarchy)
                {
                    return lsp::Null{};
                }

                features::TypeHierarchyItemRequest ir{ m_symbolTable, req.item };
                auto items = features::GetSubtypes(ir);
                if (!items.has_value() || items->empty())
                {
                    return lsp::Null{};
                }
                for (auto &item : items.value())
                {
                    EncodeItemRanges(item);
                }
                return items.value();
            });

        m_messageHandler->add<lsp::requests::TextDocument_LinkedEditingRange>(
            [this](lsp::requests::TextDocument_LinkedEditingRange::Params &&req) -> lsp::requests::TextDocument_LinkedEditingRange::Result
            {
                if (!m_config.features.enableLinkedEditing)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }

                auto scopeRoot = m_scopeIndex.GetRoot(uriStr);
                features::LinkedEditingRangeRequest lr{
                    docIt->second, scopeRoot.get(),
                    codec::Decode(docIt->second, m_positionEncoding, req.position)
                };

                auto ranges = features::GetLinkedEditingRanges(lr);
                if (!ranges.has_value())
                {
                    return lsp::Null{};
                }
                for (auto &range : ranges->ranges)
                {
                    codec::Encode(docIt->second, m_positionEncoding, range);
                }
                return ranges.value();
            });

        m_messageHandler->add<lsp::requests::TextDocument_SelectionRange>(
            [this](lsp::requests::TextDocument_SelectionRange::Params &&req) -> lsp::requests::TextDocument_SelectionRange::Result
            {
                if (!m_config.features.enableSelectionRange)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                std::vector<lsp::Position> positions;
                positions.reserve(req.positions.size());
                for (const auto &position : req.positions)
                {
                    positions.push_back(codec::Decode(docIt->second, m_positionEncoding, position));
                }

                features::SelectionRangeRequest sr{ docIt->second, tree, positions };
                auto ranges = features::GetSelectionRanges(sr);
                if (ranges.empty())
                {
                    return lsp::Null{};
                }

                // Every range in every chain is encoded, not just the outermost: each link is a
                // selection the editor will apply on its own.
                for (auto &chain : ranges)
                {
                    for (lsp::SelectionRange *link = &chain; link != nullptr; link = link->parent.get())
                    {
                        codec::Encode(docIt->second, m_positionEncoding, link->range);
                    }
                }
                return ranges;
            });

        m_messageHandler->add<lsp::requests::TextDocument_TypeDefinition>(
            [this](lsp::requests::TextDocument_TypeDefinition::Params &&req) -> lsp::requests::TextDocument_TypeDefinition::Result
            {
                if (!m_config.features.enableDefinition)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::DefinitionRequest dr{ uriStr, docIt->second, tree, m_symbolTable, m_scopeIndex, codec::Decode(docIt->second, m_positionEncoding, req.position) };
                auto defs = features::GetTypeDefinition(dr);
                if (defs.has_value() && !defs->empty())
                {
                    EncodeAcrossDocuments(defs.value());
                    return defs.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Completion>(
            [this](lsp::requests::TextDocument_Completion::Params &&req) -> lsp::requests::TextDocument_Completion::Result
            {
                if (!m_config.features.enableCompletion)
                {
                    return lsp::Array<lsp::CompletionItem>{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Array<lsp::CompletionItem>{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::CompletionRequest cr{ uriStr, docIt->second, tree, m_symbolTable, m_scopeIndex, codec::Decode(docIt->second, m_positionEncoding, req.position), &m_config, m_snippetSupport };
                return features::GetCompletion(cr);
            });

        m_messageHandler->add<lsp::requests::CompletionItem_Resolve>(
            [this](lsp::requests::CompletionItem_Resolve::Params &&req) -> lsp::requests::CompletionItem_Resolve::Result
            {
                if (!m_config.features.enableCompletion)
                {
                    return req;
                }

                features::CompletionResolveRequest rr{
                    req,
                    m_symbolTable,
                    [this](const std::string &uri) { return FindDocumentText(uri); }
                };
                return features::ResolveCompletionItem(rr);
            });

        m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Full>(
            [this](lsp::requests::TextDocument_SemanticTokens_Full::Params &&req) -> lsp::requests::TextDocument_SemanticTokens_Full::Result
            {
                if (!m_config.features.enableSemanticTokens)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }

                return ComputeAndCacheSemanticTokens(uriStr, docIt->second);
            });

        m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Full_Delta>(
            [this](lsp::requests::TextDocument_SemanticTokens_Full_Delta::Params &&req) -> lsp::requests::TextDocument_SemanticTokens_Full_Delta::Result
            {
                if (!m_config.features.enableSemanticTokens)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }

                const auto cached = m_semanticTokensCache.find(uriStr);
                const bool canDiff = cached != m_semanticTokensCache.end() &&
                                     cached->second.resultId == req.previousResultId;

                // The previous payload has to be the one this server actually sent. Anything else -
                // a reopened document, a result id from a past session - is answered with the full
                // stream, which the protocol allows in place of a delta.
                if (!canDiff)
                {
                    return ComputeAndCacheSemanticTokens(uriStr, docIt->second);
                }

                const std::vector<lsp::uint> previous = cached->second.data;
                lsp::SemanticTokens tokens = ComputeAndCacheSemanticTokens(uriStr, docIt->second);

                lsp::SemanticTokensDelta delta;
                delta.resultId = tokens.resultId;
                delta.edits = features::ComputeSemanticTokensDelta(previous, tokens.data);
                return delta;
            });

        m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Range>(
            [this](lsp::requests::TextDocument_SemanticTokens_Range::Params &&req) -> lsp::requests::TextDocument_SemanticTokens_Range::Result
            {
                if (!m_config.features.enableSemanticTokens)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::SemanticTokensRequest sr{ uriStr, docIt->second, tree, m_symbolTable, m_scopeIndex.GetRoot(uriStr) };
                // Decoded on the way in for the same reason the payload is encoded on the way out:
                // the handler works in Tree-sitter byte columns, the client speaks the negotiated
                // encoding, and a non-ASCII character earlier in the line makes them disagree.
                sr.range = codec::Decode(docIt->second, m_positionEncoding, req.range);

                lsp::SemanticTokens tokens = features::GetSemanticTokens(sr);
                codec::EncodeSemanticTokens(docIt->second, m_positionEncoding, tokens.data);
                return tokens;
            });

        m_messageHandler->add<lsp::requests::TextDocument_SignatureHelp>(
            [this](lsp::requests::TextDocument_SignatureHelp::Params &&req) -> lsp::requests::TextDocument_SignatureHelp::Result
            {
                if (!m_config.features.enableSignatureHelp)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::SignatureHelpRequest sr{ uriStr, docIt->second, tree, m_symbolTable, m_scopeIndex, codec::Decode(docIt->second, m_positionEncoding, req.position) };
                auto sig = features::GetSignatureHelp(sr);
                if (sig.has_value())
                {
                    return sig.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_DocumentSymbol>(
            [this](lsp::requests::TextDocument_DocumentSymbol::Params &&req) -> lsp::requests::TextDocument_DocumentSymbol::Result
            {
                if (!m_config.features.enableDocumentSymbols)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::DocumentSymbolRequest dr{ uriStr, docIt->second, tree, m_symbolTable };
                auto symbols = features::GetDocumentSymbols(dr);
                if (symbols.has_value())
                {
                    EncodeIn(docIt->second, symbols.value());
                    return symbols.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::Workspace_Symbol>(
            [this](lsp::requests::Workspace_Symbol::Params &&req) -> lsp::requests::Workspace_Symbol::Result
            {
                if (!m_config.features.enableWorkspaceSymbols)
                {
                    return lsp::Array<lsp::SymbolInformation>{};
                }
                features::WorkspaceSymbolRequest wr{ req.query, m_symbolTable };
                auto symbols = features::GetWorkspaceSymbols(wr);
                if (symbols.has_value())
                {
                    EncodeAcrossDocuments(symbols.value());
                    return symbols.value();
                }
                return lsp::Array<lsp::SymbolInformation>{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_References>(
            [this](lsp::requests::TextDocument_References::Params &&req) -> lsp::requests::TextDocument_References::Result
            {
                if (!m_config.features.enableReferences)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::ReferencesRequest rr{ uriStr, docIt->second, tree, codec::Decode(docIt->second, m_positionEncoding, req.position), req.context.includeDeclaration, m_symbolTable, m_scopeIndex };
                auto refs = features::GetReferences(rr);
                if (refs.has_value())
                {
                    EncodeAcrossDocuments(refs.value());
                    return refs.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_PrepareRename>(
            [this](lsp::requests::TextDocument_PrepareRename::Params &&req) -> lsp::requests::TextDocument_PrepareRename::Result
            {
                if (!m_config.features.enableRename)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                std::unordered_set<std::string> predefinedUris;
                {
                    std::lock_guard<std::mutex> lock(m_predefinedMutex);
                    predefinedUris.insert(m_predefinedUris.begin(), m_predefinedUris.end());
                }

                features::PrepareRenameRequest pr{ uriStr, docIt->second, tree, codec::Decode(docIt->second, m_positionEncoding, req.position), m_symbolTable, m_scopeIndex, predefinedUris };
                auto prep = features::PrepareRename(pr);
                if (prep.has_value())
                {
                    EncodeIn(docIt->second, prep.value());
                    return prep.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Rename>(
            [this](lsp::requests::TextDocument_Rename::Params &&req) -> lsp::requests::TextDocument_Rename::Result
            {
                if (!m_config.features.enableRename)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                std::unordered_set<std::string> predefinedUris;
                {
                    std::lock_guard<std::mutex> lock(m_predefinedMutex);
                    predefinedUris.insert(m_predefinedUris.begin(), m_predefinedUris.end());
                }

                features::RenameRequest rr{ uriStr, docIt->second, tree, codec::Decode(docIt->second, m_positionEncoding, req.position), req.newName, m_symbolTable, m_scopeIndex, predefinedUris };
                auto edit = features::Rename(rr);
                if (edit.has_value())
                {
                    EncodeAcrossDocuments(edit.value());
                    return edit.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_DocumentHighlight>(
            [this](lsp::requests::TextDocument_DocumentHighlight::Params &&req) -> lsp::requests::TextDocument_DocumentHighlight::Result
            {
                if (!m_config.features.enableDocumentHighlight)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::DocumentHighlightRequest hr{ uriStr, docIt->second, tree, codec::Decode(docIt->second, m_positionEncoding, req.position), m_symbolTable, m_scopeIndex };
                auto highlights = features::GetDocumentHighlights(hr);
                if (highlights.has_value() && !highlights->empty())
                {
                    EncodeIn(docIt->second, highlights.value());
                    return highlights.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_FoldingRange>(
            [this](lsp::requests::TextDocument_FoldingRange::Params &&req) -> lsp::requests::TextDocument_FoldingRange::Result
            {
                if (!m_config.features.enableFoldingRange)
                {
                    return lsp::Array<lsp::FoldingRange>{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Array<lsp::FoldingRange>{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::FoldingRangeRequest fr{ uriStr, docIt->second, tree };
                auto ranges = features::GetFoldingRanges(fr);
                if (ranges.has_value())
                {
                    EncodeIn(docIt->second, ranges.value());
                    return ranges.value();
                }
                return lsp::Array<lsp::FoldingRange>{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_InlayHint>(
            [this](lsp::requests::TextDocument_InlayHint::Params &&req) -> lsp::requests::TextDocument_InlayHint::Result
            {
                if (!m_config.features.enableInlayHints)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::InlayHintRequest ihr{ uriStr, docIt->second, tree, codec::Decode(docIt->second, m_positionEncoding, req.range), m_symbolTable, m_scopeIndex };
                auto hints = features::GetInlayHints(ihr);
                if (hints.has_value())
                {
                    EncodeIn(docIt->second, hints.value());
                    return hints.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_CodeAction>(
            [this](lsp::requests::TextDocument_CodeAction::Params &&req) -> lsp::requests::TextDocument_CodeAction::Result
            {
                if (!m_config.features.enableCodeAction)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                // The context's diagnostics carry ranges too, and they arrive in the client's
                // encoding exactly as req.range does. Decoding the one and not the other left the
                // handler mixing coordinate systems: every quick fix driven by a diagnostic reads
                // diag.range as a Tree-sitter point, which is only the same number while the line
                // is ASCII. Decoded here so the handler sees one system throughout.
                lsp::CodeActionContext context = req.context;
                for (auto &diag : context.diagnostics)
                    diag.range = codec::Decode(docIt->second, m_positionEncoding, diag.range);

                features::CodeActionRequest car{ uriStr, docIt->second, tree, codec::Decode(docIt->second, m_positionEncoding, req.range), context, m_symbolTable, m_scopeIndex };
                auto actions = features::GetCodeActions(car);
                if (actions.has_value())
                {
                    EncodeAcrossDocuments(*actions);
                    lsp::Array<lsp::OneOf<lsp::Command, lsp::CodeAction>> resultList;
                    resultList.reserve(actions->size());
                    for (auto &action : *actions)
                    {
                        resultList.push_back(std::move(action));
                    }
                    return resultList;
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::CodeAction_Resolve>(
            [this](lsp::requests::CodeAction_Resolve::Params &&req) -> lsp::requests::CodeAction_Resolve::Result
            {
                if (!m_config.features.enableCodeAction)
                {
                    return req;
                }

                features::CodeActionResolveRequest carr{ req, m_symbolTable, m_scopeIndex };
                auto resolved = features::ResolveCodeAction(carr);
                if (resolved.has_value())
                {
                    if (resolved->edit.has_value())
                    {
                        EncodeAcrossDocuments(resolved->edit.value());
                    }
                    return resolved.value();
                }
                return req;
            });

        m_messageHandler->add<lsp::requests::TextDocument_Formatting>(
            [this](lsp::requests::TextDocument_Formatting::Params &&req) -> lsp::requests::TextDocument_Formatting::Result
            {
                if (!m_config.features.enableFormatting)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::FormattingRequest fr{ uriStr, docIt->second, tree, req.options, CurrentBraceStyle() };
                auto edits = features::FormatDocument(fr);
                if (edits.has_value())
                {
                    EncodeIn(docIt->second, edits.value());
                    return edits.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_DocumentLink>(
            [this](lsp::requests::TextDocument_DocumentLink::Params &&req) -> lsp::requests::TextDocument_DocumentLink::Result
            {
                if (!m_config.features.enableDocumentLink)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }

                // Named so the handle outlives dlr, which holds the vector by reference.
                const auto searchDirectories = SearchDirectories();

                features::DocumentLinkRequest dlr{ uriStr, docIt->second, *searchDirectories, m_i18n.get(), IncludeAllowedRoots() };
                auto links = features::GetDocumentLinks(dlr);
                if (links.has_value())
                {
                    EncodeIn(docIt->second, links.value());
                    return links.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_RangeFormatting>(
            [this](lsp::requests::TextDocument_RangeFormatting::Params &&req) -> lsp::requests::TextDocument_RangeFormatting::Result
            {
                if (!m_config.features.enableFormatting)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::RangeFormattingRequest rfr{ uriStr, docIt->second, tree, codec::Decode(docIt->second, m_positionEncoding, req.range), req.options, CurrentBraceStyle() };
                auto edits = features::FormatRange(rfr);
                if (edits.has_value())
                {
                    EncodeIn(docIt->second, edits.value());
                    return edits.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_CodeLens>(
            [this](lsp::requests::TextDocument_CodeLens::Params &&req) -> lsp::requests::TextDocument_CodeLens::Result
            {
                if (!m_config.features.enableCodeLens)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::CodeLensRequest clr{ uriStr, docIt->second, tree, m_symbolTable, m_scopeIndex };
                auto lenses = features::GetCodeLenses(clr);
                if (lenses.has_value())
                {
                    EncodeIn(docIt->second, lenses.value());
                    return lenses.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::CodeLens_Resolve>(
            [this](lsp::requests::CodeLens_Resolve::Params &&req) -> lsp::requests::CodeLens_Resolve::Result
            {
                if (!m_config.features.enableCodeLens)
                {
                    return req;
                }

                features::CodeLensResolveRequest clrr{ req, m_symbolTable, m_scopeIndex };
                auto resolved = features::ResolveCodeLens(clrr);
                return resolved.value_or(std::move(req));
            });

        m_messageHandler->add<lsp::requests::TextDocument_OnTypeFormatting>(
            [this](lsp::requests::TextDocument_OnTypeFormatting::Params &&req) -> lsp::requests::TextDocument_OnTypeFormatting::Result
            {
                if (!m_config.features.enableOnTypeFormatting)
                {
                    return lsp::Null{};
                }
                std::string uriStr = DocumentKey(req.textDocument.uri.toString());
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::OnTypeFormattingRequest otfr{
                    uriStr,
                    docIt->second,
                    tree,
                    codec::Decode(docIt->second, m_positionEncoding, req.position),
                    req.ch,
                    req.options,
                    CurrentBraceStyle()
                };
                auto edits = features::FormatOnType(otfr);
                if (edits.has_value())
                {
                    EncodeIn(docIt->second, edits.value());
                    return edits.value();
                }
                return lsp::Null{};
            });
    }
}