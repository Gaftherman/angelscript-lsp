#include "Server.h"
#include "utils/Utils.h"
#include "lsp/PositionCodec.h"
#include "features/hover/HoverHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/implementation/ImplementationHandler.h"
#include "features/call_hierarchy/CallHierarchyHandler.h"
#include "features/type_hierarchy/TypeHierarchyHandler.h"
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

        m_parser = std::make_unique<angel_lsp::parser::AngelScriptParser>(m_logger.get());

        m_symbolCollector = std::make_unique<angel_lsp::analysis::SymbolCollector>(m_logger.get());

        m_localScopeCollector = std::make_unique<angel_lsp::analysis::LocalScopeCollector>(m_logger.get());

        m_semanticAnalyzer = std::make_unique<angel_lsp::analysis::SemanticAnalyzer>(m_logger.get());

        m_i18n = std::make_unique<angel_lsp::i18n::I18n>(m_config.info.locale.empty() ? "en" : m_config.info.locale);

        BuildDiagnosticSeverityOverrides();

        InitHandles();

        m_analysisThread = std::jthread([this](std::stop_token stopToken)
                                       { this->RunAnalysisLoop(stopToken); });
    }

    Server::~Server()
    {
        m_running = false;

        // Stopped and joined before the trees below are freed: both threads read member state and
        // must not outlive it. jthread would join on destruction anyway, but that happens in
        // reverse declaration order - and m_workspaceThread is declared early enough that it would
        // be joined only after the symbol table and include graph it reads were already gone.
        m_analysisThread.request_stop();
        if (m_analysisThread.joinable())
            m_analysisThread.join();

        m_workspaceThread.request_stop();
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

    void Server::Run()
    {
        while (m_running)
        {
            // A closed transport is how this process normally ends: the editor exits, stdin hits
            // end of file, and the framework reports it by throwing. Letting that escape main()
            // would turn an ordinary shutdown into a crash, and the destructors that join the
            // background threads would never run.
            try
            {
                m_messageHandler->processIncomingMessages();
            }
            catch (const lsp::ConnectionError &e)
            {
                m_logger->LogInfo(fmt::format("Connection closed: {}", e.what()));
                m_running = false;
            }
            catch (const lsp::io::Error &e)
            {
                m_logger->LogInfo(fmt::format("Transport closed: {}", e.what()));
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
                m_workspacesRoot.push_back(std::string(workspace.uri.path()));
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
            result.capabilities.codeActionProvider = true;
        }

        if (m_config.features.enableFormatting)
        {
            result.capabilities.documentFormattingProvider = true;
            result.capabilities.documentRangeFormattingProvider = true;
        }

        if (m_config.features.enableDocumentLink)
        {
            lsp::DocumentLinkOptions linkOpts;
            linkOpts.resolveProvider = false;
            result.capabilities.documentLinkProvider = linkOpts;
        }

        // Announced unconditionally: both the include graph and the predefined-stub scan are scoped
        // to the known roots, so a folder added mid-session has to reach the server whatever else
        // is switched off.
        lsp::WorkspaceFoldersServerCapabilities folderCaps;
        folderCaps.supported = true;
        folderCaps.changeNotifications = true;

        lsp::WorkspaceOptions workspaceOpts;
        workspaceOpts.workspaceFolders = folderCaps;
        result.capabilities.workspace = workspaceOpts;

        return result;
    }

    void Server::HandleNotificationsInitialized(lsp::notifications::Initialized::Params &&params)
    {
        // Started unconditionally: even with the predefined-stub loader disabled, the workspace
        // thread still has to build the #include graph that module-closure indexing depends on.
        {
            m_workspaceThread = std::jthread([this](std::stop_token stopToken)
                                             { this->ReadWorkspaceFiles(stopToken); });
        }
    }

    void Server::ReadWorkspaceFiles(std::stop_token stopToken)
    {
        // The #include graph is what decides which files get indexed alongside an opened document,
        // so it is built regardless of the predefined-stub loader below. Only directives are parsed
        // here, never the AST, which is what keeps a full-workspace scan affordable at startup.
        std::vector<std::string> roots;
        roots.reserve(m_workspacesRoot.size());
        for (const auto &workspaceRoot : m_workspacesRoot)
            roots.push_back(angel_lsp::utils::UriToPath(workspaceRoot));

        m_includeGraph.Build(roots,
                             m_config.searchDirectories,
                             m_config.info.fileExtension,
                             [&stopToken]() { return stopToken.stop_requested(); });

        if (stopToken.stop_requested())
            return;

        m_logger->LogInfo(fmt::format("Include graph built: {} script file(s)", m_includeGraph.FileCount()));

        if (!m_config.features.enablePredefinedLoader)
        {
            return;
        }

        angel_lsp::parser::AngelScriptParser backgroundParser(m_logger.get());

        // Explicitly configured stubs first. A host application's declarations usually ship with
        // the application, not with the scripts, so the scan below - which only ever walks
        // workspace folders - would never find them. ParserPredefined de-duplicates by canonical
        // path, so a stub that also happens to live inside the workspace is not indexed twice.
        LoadConfiguredPredefinedFiles(backgroundParser, stopToken);

        if (stopToken.stop_requested())
            return;

        try
        {
            for (const auto &workspaceRoot : m_workspacesRoot)
            {
                if (stopToken.stop_requested())
                    return;

                for (const auto &entry : std::filesystem::recursive_directory_iterator(angel_lsp::utils::UriToPath(workspaceRoot)))
                {
                    if (stopToken.stop_requested())
                        return;

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
    }

    void Server::LoadConfiguredPredefinedFiles(angel_lsp::parser::AngelScriptParser &parser, std::stop_token stopToken)
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
                for (const auto &workspaceRoot : m_workspacesRoot)
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

        m_symbolTable.ClearDocumentSymbols(uri);
        m_symbolCollector->CollectSymbols(uri, content, parser, m_symbolTable);

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

        const auto *directories = section->find("searchDirectories");
        if (!directories || !directories->isArray())
        {
            return;
        }

        std::vector<std::string> updated;
        for (const auto &entry : directories->array())
        {
            if (entry.isString() && !entry.string().empty())
                updated.push_back(entry.string());
        }

        if (updated == m_config.searchDirectories)
        {
            return;
        }

        m_config.searchDirectories = std::move(updated);
        m_logger->LogInfo(fmt::format("Search directories changed ({} entries); rebuilding the include graph", m_config.searchDirectories.size()));

        // Which files a directive resolves to depends entirely on these paths, so every edge in the
        // graph is now suspect.
        RestartWorkspaceScan();
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

    void Server::RestartWorkspaceScan()
    {
        // Assigning over a running jthread requests its stop and joins it, so the scan in flight
        // ends at its next stop check rather than racing the new one. Run on the workspace thread
        // for the same reason it is at startup: a full scan must not block the message loop.
        m_workspaceThread = std::jthread([this](std::stop_token stopToken)
                                         { this->ReadWorkspaceFiles(stopToken); });
    }

    void Server::HandleNotificationsWorkspace_DidChangeWorkspaceFolders(lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params &&params)
    {
        for (const auto &removed : params.event.removed)
        {
            const std::string root{ removed.uri.path() };
            std::erase(m_workspacesRoot, root);
        }

        for (const auto &added : params.event.added)
        {
            const std::string root{ added.uri.path() };
            if (std::find(m_workspacesRoot.begin(), m_workspacesRoot.end(), root) == m_workspacesRoot.end())
                m_workspacesRoot.push_back(root);
        }

        m_logger->LogInfo(fmt::format("Workspace folders changed (+{} -{}); now {} root(s), rescanning",
                                      params.event.added.size(), params.event.removed.size(), m_workspacesRoot.size()));

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
            const std::string uriStr = event.uri.toString();

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
                    ParserPredefined(path, watchedParser, /*forceReload=*/true);
                continue;
            }

            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
                continue;

            const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            m_includeGraph.UpdateFile(path, content, m_config.searchDirectories);
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
        request.severityOverrides = m_diagnosticSeverities.empty() ? nullptr : &m_diagnosticSeverities;
        request.enableTypeConversionChecks = m_config.features.enableTypeConversionChecks;
        request.scopeRoot = m_scopeIndex.GetRoot(uriStr);
        request.sourceCode = text;
        request.tree = tree;
        return request;
    }

    void Server::HandleNotificationsTextDocument_DidSave(lsp::notifications::TextDocument_DidSave::Params &&params)
    {
        std::string uriStr = params.textDocument.uri.toString();
        std::string text = params.text.has_value() ? params.text.value() : "";

        if (text.empty() && m_openDocuments.contains(uriStr))
            text = m_openDocuments[uriStr];

        m_symbolTable.ClearDocumentSymbols(uriStr);
        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            {
                std::lock_guard<std::mutex> lock(m_predefinedMutex);
                // The return value is deliberately ignored: a save always re-collects, and the call
                // is here for its other effect - dropping any copy the workspace scan indexed under
                // a different URI spelling of this same file.
                ClaimPredefinedFile(uriStr);

                m_symbolCollector->CollectSymbols(uriStr, text, *m_parser, m_symbolTable);
                m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopes(text, *m_parser));
            }
            PublishDiagnostics(uriStr, {});
            return;
        }

        // Parsed once and shared: symbol collection, scope building and the conversion rules all
        // need the same tree, and letting each of them parse the text again is pure waste.
        TSTree *savedTree = m_parser->Parse(text);

        auto diagnostics = m_symbolCollector->CollectSymbolsWithTree(uriStr, text, savedTree, m_symbolTable, m_i18n.get(), &m_config.types);
        if (savedTree)
        {
            m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(savedTree), text));
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(savedTree), text));
        }

        // A save is the only point at which an edited #include line can change which module this
        // file belongs to, so the graph is patched here rather than on every keystroke.
        if (const std::string savedPath = CanonicalPathFromUri(uriStr); !savedPath.empty())
            m_includeGraph.UpdateFile(savedPath, text, m_config.searchDirectories);

        IndexModuleClosure(uriStr);

        auto semanticDiagnostics = m_semanticAnalyzer->Analyze(BuildAnalysisRequest(uriStr, text, savedTree));
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

        if (savedTree)
            ts_tree_delete(savedTree);

        AppendIncludeDiagnostics(uriStr, text, diagnostics);

        PublishDiagnostics(uriStr, diagnostics);
    }

    void Server::HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params &&params)
    {
        std::string uriStr = params.textDocument.uri.toString();
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
            {
                std::lock_guard<std::mutex> lock(m_predefinedMutex);
                // Claims by canonical path, so opening a stub the workspace scan already loaded
                // under its own URI spelling hands ownership over instead of indexing it twice.
                if (ClaimPredefinedFile(uriStr))
                {
                    m_symbolTable.ClearDocumentSymbols(uriStr);
                    m_symbolCollector->CollectSymbolsWithTree(uriStr, text, tree, m_symbolTable, m_i18n.get());
                    m_scopeIndex.ClearDocument(uriStr);
                    m_callGraph.ClearDocument(uriStr);
                    if (tree)
                    {
                        m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(tree), text));
                        m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(tree), text));
                    }
                }
            }
            PublishDiagnostics(uriStr, {});
            return;
        }

        m_symbolTable.ClearDocumentSymbols(uriStr);
        auto diagnostics = m_symbolCollector->CollectSymbolsWithTree(uriStr, text, tree, m_symbolTable, m_i18n.get(), &m_config.types);

        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);
        if (tree)
        {
            m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(tree), text));
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(tree), text));
        }

        // Before analysis, not after: the module the file belongs to supplies declarations this
        // file legitimately uses, and without them every one of them would be reported undeclared.
        IndexModuleClosure(uriStr);

        auto semanticDiagnostics = m_semanticAnalyzer->Analyze(BuildAnalysisRequest(uriStr, text, tree));
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

        AppendIncludeDiagnostics(uriStr, text, diagnostics);

        PublishDiagnostics(uriStr, diagnostics);
    }

    void Server::HandleNotificationsTextDocument_DidChange(lsp::notifications::TextDocument_DidChange::Params &&params)
    {
        std::string uriStr = params.textDocument.uri.toString();
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
            m_symbolCollector->CollectSymbolsWithTree(uriStr, buffer, newTree, m_symbolTable, m_i18n.get());
            if (newTree)
            {
                m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(newTree), buffer));
                m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(newTree), buffer));
            }
            PublishDiagnostics(uriStr, {});
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
        std::string uriStr = params.textDocument.uri.toString();
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

    std::string Server::UriFromPath(const std::string &path)
    {
        return lsp::Uri::fileUriFromPath(path).toString();
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

        m_symbolTable.ClearDocumentSymbols(uriStr);
        m_symbolCollector->CollectSymbolsWithTree(uriStr, content, tree, m_symbolTable, m_i18n.get());

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

    void Server::RunAnalysisLoop(std::stop_token stopToken)
    {
        std::stop_callback wake(stopToken, [this]()
                                {
                                    {
                                        std::lock_guard<std::mutex> lock(m_analysisMutex);
                                        m_analysisStop = true;
                                    }
                                    m_analysisCv.notify_all();
                                });

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
                AnalyzeDocument(uriStr, text);
        }
    }

    void Server::AnalyzeDocument(const std::string &uriStr, const std::string &text)
    {
        // Own parser, own tree, own copy of the text. The message loop owns m_documentTrees and
        // deletes the tree there on the next edit; reading it from here would be a use-after-free.
        angel_lsp::parser::AngelScriptParser parser(m_logger.get());
        TSTree *tree = parser.Parse(text);

        // Collected into a staging table and swapped in one step, so a reader on the message loop
        // never catches this document mid-rebuild with no symbols at all.
        angel_lsp::analysis::SymbolTable staging;
        auto diagnostics = m_symbolCollector->CollectSymbolsWithTree(uriStr, text, tree, staging, m_i18n.get(), &m_config.types);
        m_symbolTable.ReplaceDocumentSymbols(uriStr, staging);

        if (tree)
        {
            m_scopeIndex.SetScopeTree(uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(tree), text));
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(tree), text));
        }
        else
            m_scopeIndex.ClearDocument(uriStr);
            m_callGraph.ClearDocument(uriStr);

        // Deleted after analysis, not before: the conversion rules read expressions straight out of
        // this tree, and it is the only one this thread is allowed to touch.
        auto semanticDiagnostics = m_semanticAnalyzer->Analyze(BuildAnalysisRequest(uriStr, text, tree));
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
        features::DocumentLinkRequest request{ uriStr, text, m_config.searchDirectories, m_i18n.get() };
        auto includeDiagnostics = features::GetUnresolvedIncludeDiagnostics(request);
        diagnostics.insert(diagnostics.end(), includeDiagnostics.begin(), includeDiagnostics.end());
    }

    void Server::PurgeClosureFile(const std::string &uriStr)
    {
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
            if (const std::string *text = FindDocumentText(location.uri.toString()))
                codec::Encode(*text, m_positionEncoding, location.range);
        }
    }

    void Server::EncodeAcrossDocuments(std::vector<lsp::SymbolInformation> &symbols) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8)
            return;

        for (auto &symbol : symbols)
        {
            if (const std::string *text = FindDocumentText(symbol.location.uri.toString()))
                codec::Encode(*text, m_positionEncoding, symbol.location.range);
        }
    }

    void Server::EncodeAcrossDocuments(lsp::WorkspaceEdit &edit) const
    {
        if (m_positionEncoding == angel_lsp::utils::PositionEncoding::Utf8 || !edit.changes.has_value())
            return;

        // Only the `changes` form is produced (see RenameHandler.cpp and CodeActionHandler.cpp);
        // documentChanges is left alone so that a future switch to it fails loudly in review
        // rather than silently shipping unconverted ranges.
        for (auto &[uri, edits] : edit.changes.value())
        {
            if (const std::string *text = FindDocumentText(uri.toString()))
                EncodeIn(*text, edits);
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

    void Server::PublishDiagnostics(const std::string &uriStr, const std::string &text, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics)
    {
        lsp::notifications::TextDocument_PublishDiagnostics::Params params;
        params.uri = lsp::DocumentUri(lsp::Uri::parse(uriStr));

        for (const auto &diag : diagnostics)
        {
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

            params.diagnostics.push_back(lspDiag);
        }

        std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
        m_messageHandler->sendNotification<lsp::notifications::TextDocument_PublishDiagnostics>(std::move(params));
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
                std::string uriStr = req.textDocument.uri.toString();
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::HoverRequest hr{ uriStr, docIt->second, tree, m_symbolTable, m_scopeIndex, codec::Decode(docIt->second, m_positionEncoding, req.position) };
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                    EncodeRangesIn(call.from.uri.toString(), call.fromRanges);
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
                const std::string callerUri = req.item.uri.toString();

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
                std::string uriStr = req.textDocument.uri.toString();
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

        m_messageHandler->add<lsp::requests::TextDocument_SelectionRange>(
            [this](lsp::requests::TextDocument_SelectionRange::Params &&req) -> lsp::requests::TextDocument_SelectionRange::Result
            {
                if (!m_config.features.enableSelectionRange)
                {
                    return lsp::Null{};
                }
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Array<lsp::CompletionItem>{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::CompletionRequest cr{ uriStr, docIt->second, tree, m_symbolTable, m_scopeIndex, codec::Decode(docIt->second, m_positionEncoding, req.position), &m_config };
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
                std::string uriStr = req.textDocument.uri.toString();
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                return ComputeAndCacheSemanticTokens(uriStr, docIt->second);
            });

        m_messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Full_Delta>(
            [this](lsp::requests::TextDocument_SemanticTokens_Full_Delta::Params &&req) -> lsp::requests::TextDocument_SemanticTokens_Full_Delta::Result
            {
                if (!m_config.features.enableSemanticTokens)
                {
                    return lsp::Null{};
                }
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
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
                std::string uriStr = req.textDocument.uri.toString();
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::CodeActionRequest car{ uriStr, docIt->second, tree, codec::Decode(docIt->second, m_positionEncoding, req.range), req.context, m_symbolTable, m_scopeIndex };
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

        m_messageHandler->add<lsp::requests::TextDocument_Formatting>(
            [this](lsp::requests::TextDocument_Formatting::Params &&req) -> lsp::requests::TextDocument_Formatting::Result
            {
                if (!m_config.features.enableFormatting)
                {
                    return lsp::Null{};
                }
                std::string uriStr = req.textDocument.uri.toString();
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::FormattingRequest fr{ uriStr, docIt->second, tree, req.options };
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
                std::string uriStr = req.textDocument.uri.toString();
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }

                features::DocumentLinkRequest dlr{ uriStr, docIt->second, m_config.searchDirectories, m_i18n.get() };
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
                std::string uriStr = req.textDocument.uri.toString();
                auto docIt = m_openDocuments.find(uriStr);
                if (docIt == m_openDocuments.end())
                {
                    return lsp::Null{};
                }
                TSTree *tree = m_documentTrees.contains(uriStr) ? m_documentTrees[uriStr] : nullptr;

                features::RangeFormattingRequest rfr{ uriStr, docIt->second, tree, codec::Decode(docIt->second, m_positionEncoding, req.range), req.options };
                auto edits = features::FormatRange(rfr);
                if (edits.has_value())
                {
                    EncodeIn(docIt->second, edits.value());
                    return edits.value();
                }
                return lsp::Null{};
            });
    }
}