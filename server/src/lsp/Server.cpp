/**
 * @file Server.cpp
 * @brief Implementation of main LSP Server JSON-RPC message dispatcher and request handlers.
 * @ingroup Server
 */

#include "Server.h"
#include <spdlog/spdlog.h>
#include <memory>
#include <shared_mutex>
#include <ankerl/unordered_dense.h>
#include "document/Document.h"
#include "analysis/ValidationOracle.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "analysis/PredefinedLoader.h"

#include "features/hover/HoverHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/completion/CompletionHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "features/signature_help/SignatureHelpHandler.h"

#include <lsp/messages.h>
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/messagehandler.h>
#include "utils/LspLogger.h"

namespace angel_lsp
{
    using namespace ::lsp;

    static std::string NormalizeUri(const std::string &rawUri)
    {
        std::string out = rawUri;
        size_t pos = 0;
        while ((pos = out.find("%3A", pos)) != std::string::npos) {
            out.replace(pos, 3, ":");
            pos += 1;
        }
        pos = 0;
        while ((pos = out.find("%3a", pos)) != std::string::npos) {
            out.replace(pos, 3, ":");
            pos += 1;
        }
        return out;
    }



    Server::Server(ServerConfig config)
        : m_config(std::move(config))
    {
        asEngine = asCreateScriptEngine();
        oracle = std::make_unique<analysis::ValidationOracle>(asEngine);
        m_diagCache = std::make_unique<analysis::DiagnosticCache>();

        analysis::PredefinedLoader::RegisterDefaultPredefined(asEngine, m_globalSymbolTable);

        m_connection = std::make_unique<lsp::Connection>(lsp::io::standardIO());
        messageHandler = std::make_unique<lsp::MessageHandler>(*m_connection);

        LspLogger::Initialize(messageHandler.get());

        RegisterHandlers();

        m_validationThread = std::jthread([this](std::stop_token st)
                                          { ValidationWorkerLoop(std::move(st)); });
    }

    Server::~Server()
    {
        m_validationThread.request_stop();
        m_validationCV.notify_all();

        if (asEngine)
        {
            asEngine->ShutDownAndRelease();
        }
    }

    void Server::RegisterHandlers()
    {
        messageHandler->add<lsp::requests::Initialize>(
            [this](lsp::requests::Initialize::Params &&params)
            {
                if (params.locale.has_value())
                {
                    m_locale = i18n::ParseLocale(params.locale.value());
                    if (oracle)
                    {
                        oracle->SetLocale(m_locale);
                    }
                }
                if (!params.rootUri.isNull())
                {
                    m_workspaceRoot = std::string(params.rootUri.value().path());
                }

                lsp::requests::Initialize::Result result;
                result.serverInfo = lsp::InitializeResultServerInfo{
                    .name = "AngelScript LSP",
                    .version = "1.0.0"};

                result.capabilities = {};

                lsp::TextDocumentSyncOptions sync;
                sync.openClose = true;
                sync.change = lsp::TextDocumentSyncKind::Full;
                result.capabilities.textDocumentSync = sync;

                if (m_config.features.enableCompletion)
                {
                    lsp::CompletionOptions completion;
                    completion.triggerCharacters = std::vector<std::string>{".", ":"};
                    result.capabilities.completionProvider = completion;
                }

                if (m_config.features.enableHover)
                {
                    result.capabilities.hoverProvider = true;
                }

                if (m_config.features.enableDefinition)
                {
                    result.capabilities.definitionProvider = true;
                    result.capabilities.typeDefinitionProvider = true;
                }

                if (m_config.features.enableSemanticTokens)
                {
                    lsp::SemanticTokensOptions semantic_options;
                    semantic_options.legend.tokenTypes = features::SemanticTokensHandler::GetTokenTypesLegend();
                    semantic_options.legend.tokenModifiers = features::SemanticTokensHandler::GetTokenModifiersLegend();
                    semantic_options.full = true;
                    result.capabilities.semanticTokensProvider = semantic_options;
                }

                if (m_config.features.enableSignatureHelp)
                {
                    lsp::SignatureHelpOptions signature_options;
                    signature_options.triggerCharacters = std::vector<std::string>{"(", ","};
                    result.capabilities.signatureHelpProvider = signature_options;
                }

                return result;
            });

        messageHandler->add<lsp::notifications::Initialized>(
            [this](lsp::notifications::Initialized::Params &&)
            {
                LspLogger::Info("Client initialized.");

                if (m_config.features.enablePredefinedLoader && !m_workspaceRoot.empty())
                {
                    m_predefinedThread = std::jthread([this](std::stop_token st)
                                                      {
                    analysis::PredefinedLoader loader;
                    
                    std::lock_guard<std::mutex> engineLock(m_engineMutex);
                    
                    auto logger = [](const std::string& msg, int severity)
                    {
                        if (severity == 1) LspLogger::Error(msg);
                        else if (severity == 2) LspLogger::Warn(msg);
                        else LspLogger::Info(msg);
                    };
                    
                    bool loaded = loader.FindInWorkspace(m_workspaceRoot, asEngine, m_globalSymbolTable, "string", "array", logger);
                    if (loaded)
                    {
                        LspLogger::Info("Loaded as.predefined successfully.");
                    }
                    else
                    {
                        LspLogger::Warn("as.predefined not found or failed to load.");
                    } });
                }
            });

        messageHandler->add<lsp::requests::Shutdown>(
            [this]()
            {
                running = false;
                return lsp::requests::Shutdown::Result{};
            });

        messageHandler->add<lsp::notifications::Exit>(
            [this]()
            {
                running = false;
            });

        messageHandler->add<lsp::notifications::Workspace_DidChangeConfiguration>(
            [this](lsp::notifications::Workspace_DidChangeConfiguration::Params &&params)
            {
                LspLogger::Info("Configuration updated from client.");
            });

        messageHandler->add<lsp::notifications::TextDocument_DidSave>(
            [this](lsp::notifications::TextDocument_DidSave::Params &&params)
            {
                std::string uri = params.textDocument.uri.toString();
                if (uri.find("as.predefined") != std::string::npos)
                {
                    lsp::requests::Window_ShowMessageRequest::Params p;
                    p.type = lsp::MessageType::Info;
                    p.message = "as.predefined was modified. Reload server to apply changes?";

                    lsp::MessageActionItem action;
                    action.title = "Reload Window";
                    p.actions = std::vector<lsp::MessageActionItem>{action};

                    messageHandler->sendRequest<lsp::requests::Window_ShowMessageRequest>(
                        std::move(p),
                        [this](lsp::requests::Window_ShowMessageRequest::Result &&res)
                        {
                            if (!res.isNull() && res.value().title == "Reload Window")
                            {
                                std::lock_guard<std::mutex> engineLock(m_engineMutex);
                                if (asEngine)
                                    asEngine->ShutDownAndRelease();
                                asEngine = asCreateScriptEngine();
                                oracle = std::make_unique<analysis::ValidationOracle>(asEngine);
                                m_globalSymbolTable.ClearAll();

                                analysis::PredefinedLoader loader;
                                auto logger = [](const std::string &msg, int severity)
                                {
                                    if (severity == 1)
                                        LspLogger::Error(msg);
                                    else if (severity == 2)
                                        LspLogger::Warn(msg);
                                    else
                                        LspLogger::Info(msg);
                                };
                                loader.FindInWorkspace(m_workspaceRoot, asEngine, m_globalSymbolTable, "string", "array", logger);
                                LspLogger::Info("Server reloaded due to as.predefined save.");
                            }
                        });
                }
            });

        messageHandler->add<lsp::notifications::TextDocument_DidOpen>(
            [this](lsp::notifications::TextDocument_DidOpen::Params &&params)
            {
                std::string uri = NormalizeUri(params.textDocument.uri.toString());
                std::string text = params.textDocument.text;

                auto doc = std::make_unique<Document>(uri, text);
                ScheduleValidation(uri, text);

                std::unique_lock lock(m_docMutex);
                m_documents[uri] = std::move(doc);

                auto docResolver = [this](const std::string &u) -> const Document *
                {
                    auto it = m_documents.find(NormalizeUri(u));
                    if (it != m_documents.end())
                    {
                        return it->second.get();
                    }
                    return nullptr;
                };

                auto &table = m_symbolTables[uri];
                table.ClearAll();
                table.MergeGlobals(m_globalSymbolTable);
                analysis::SymbolCollector::CollectGlobals(*m_documents[uri], table, docResolver);
                analysis::SymbolCollector::CollectLocals(*m_documents[uri], table);
            });

        messageHandler->add<lsp::notifications::TextDocument_DidChange>(
            [this](lsp::notifications::TextDocument_DidChange::Params &&params)
            {
                std::string uri = NormalizeUri(params.textDocument.uri.toString());

                std::unique_lock lock(m_docMutex);
                auto it = m_documents.find(uri);
                if (it == m_documents.end())
                {
                    return;
                }

                if (!params.contentChanges.empty())
                {
                    if (const auto *change = std::get_if<lsp::TextDocumentContentChangeEvent_Text>(&params.contentChanges[0]))
                    {
                        std::string newText = change->text;
                        it->second = std::make_unique<Document>(uri, newText);

                        auto docResolver = [this](const std::string &u) -> const Document *
                        {
                            auto dIt = m_documents.find(NormalizeUri(u));
                            if (dIt != m_documents.end())
                            {
                                return dIt->second.get();
                            }
                            return nullptr;
                        };

                        auto &table = m_symbolTables[uri];
                        table.ClearAll();
                        table.MergeGlobals(m_globalSymbolTable);
                        analysis::SymbolCollector::CollectGlobals(*it->second, table, docResolver);
                        analysis::SymbolCollector::CollectLocals(*it->second, table);

                        ScheduleValidation(uri, newText);
                    }
                }
            });

        messageHandler->add<lsp::notifications::TextDocument_DidClose>(
            [this](lsp::notifications::TextDocument_DidClose::Params &&params)
            {
                std::string uri = NormalizeUri(params.textDocument.uri.toString());

                std::unique_lock lock(m_docMutex);
                m_documents.erase(uri);
                m_symbolTables.erase(uri);
            });

        // Feature Handlers
        messageHandler->add<lsp::requests::TextDocument_Hover>(
            [this](lsp::requests::TextDocument_Hover::Params &&req)
            {
                if (!m_config.features.enableHover)
                {
                    return lsp::requests::TextDocument_Hover::Result{};
                }

                std::string uri = NormalizeUri(req.textDocument.uri.toString());
                std::shared_lock lock(m_docMutex);
                auto docIt = m_documents.find(uri);
                auto tableIt = m_symbolTables.find(uri);
                if (docIt != m_documents.end() && tableIt != m_symbolTables.end())
                {
                    lsp::requests::TextDocument_Hover::Result result;
                    features::ProcessHover(result, req, *docIt->second, tableIt->second, m_diagCache.get(), m_locale, asEngine);
                    return result;
                }
                return lsp::requests::TextDocument_Hover::Result{};
            });

        messageHandler->add<lsp::requests::TextDocument_Definition>(
            [this](lsp::requests::TextDocument_Definition::Params &&req)
            {
                if (!m_config.features.enableDefinition)
                {
                    return lsp::requests::TextDocument_Definition::Result{};
                }

                std::string uri = NormalizeUri(req.textDocument.uri.toString());
                std::shared_lock lock(m_docMutex);
                auto docIt = m_documents.find(uri);
                auto tableIt = m_symbolTables.find(uri);
                if (docIt != m_documents.end() && tableIt != m_symbolTables.end())
                {
                    return features::ProcessDefinition(req, *docIt->second, tableIt->second, asEngine);
                }
                return lsp::requests::TextDocument_Definition::Result{};
            });

        messageHandler->add<lsp::requests::TextDocument_TypeDefinition>(
            [this](lsp::requests::TextDocument_TypeDefinition::Params &&req)
            {
                if (!m_config.features.enableDefinition)
                {
                    return lsp::requests::TextDocument_TypeDefinition::Result{};
                }

                std::string uri = NormalizeUri(req.textDocument.uri.toString());
                std::shared_lock lock(m_docMutex);
                auto docIt = m_documents.find(uri);
                auto tableIt = m_symbolTables.find(uri);
                if (docIt != m_documents.end() && tableIt != m_symbolTables.end())
                {
                    return features::ProcessTypeDefinition(req, *docIt->second, tableIt->second, asEngine);
                }
                return lsp::requests::TextDocument_TypeDefinition::Result{};
            });

        messageHandler->add<lsp::requests::TextDocument_Completion>(
            [this](lsp::requests::TextDocument_Completion::Params &&req)
            {
                if (!m_config.features.enableCompletion)
                {
                    return lsp::requests::TextDocument_Completion::Result{};
                }

                std::string uri = NormalizeUri(req.textDocument.uri.toString());
                std::shared_lock lock(m_docMutex);
                auto docIt = m_documents.find(uri);
                auto tableIt = m_symbolTables.find(uri);
                if (docIt != m_documents.end() && tableIt != m_symbolTables.end())
                {
                    return features::ProcessCompletion(req, *docIt->second, tableIt->second, asEngine);
                }
                return lsp::requests::TextDocument_Completion::Result{};
            });

        messageHandler->add<lsp::requests::TextDocument_SemanticTokens_Full>(
            [this](lsp::requests::TextDocument_SemanticTokens_Full::Params &&req)
            {
                if (!m_config.features.enableSemanticTokens)
                {
                    return lsp::requests::TextDocument_SemanticTokens_Full::Result{};
                }

                std::string uri = NormalizeUri(req.textDocument.uri.toString());
                std::shared_lock lock(m_docMutex);
                auto docIt = m_documents.find(uri);
                auto tableIt = m_symbolTables.find(uri);
                if (docIt != m_documents.end() && tableIt != m_symbolTables.end())
                {
                    return features::ProcessSemanticTokensFull(req, *docIt->second, tableIt->second);
                }
                return lsp::requests::TextDocument_SemanticTokens_Full::Result{};
            });

        messageHandler->add<lsp::requests::TextDocument_SignatureHelp>(
            [this](lsp::requests::TextDocument_SignatureHelp::Params &&req)
            {
                if (!m_config.features.enableSignatureHelp)
                {
                    return lsp::requests::TextDocument_SignatureHelp::Result{};
                }

                std::string uri = NormalizeUri(req.textDocument.uri.toString());
                std::shared_lock lock(m_docMutex);
                auto docIt = m_documents.find(uri);
                auto tableIt = m_symbolTables.find(uri);
                if (docIt != m_documents.end() && tableIt != m_symbolTables.end())
                {
                    return features::ProcessSignatureHelp(req, *docIt->second, tableIt->second, asEngine);
                }
                return lsp::requests::TextDocument_SignatureHelp::Result{};
            });
    }

    void Server::Run()
    {
        while (running)
        {
            messageHandler->processIncomingMessages();
        }
    }

    void Server::ScheduleValidation(std::string uri, std::string text)
    {
        std::unique_lock lock(m_validationMutex);
        m_pendingUri = std::move(uri);
        m_pendingText = std::move(text);
        m_validationPending = true;
        m_validationCV.notify_one();
    }

    void Server::ValidationWorkerLoop(std::stop_token st)
    {
        while (!st.stop_requested())
        {
            std::unique_lock lock(m_validationMutex);

            m_validationCV.wait(lock, [this, &st]
                                { return m_validationPending || st.stop_requested(); });
            if (st.stop_requested())
                break;

            // Debounce: wait for 300ms
            m_validationCV.wait_for(lock, std::chrono::milliseconds(300), [&st]
                                    { return st.stop_requested(); });
            if (st.stop_requested())
                break;

            if (!m_validationPending)
                continue; // consumed

            std::string uri = NormalizeUri(std::move(m_pendingUri));
            std::string text = std::move(m_pendingText);
            m_validationPending = false;

            lock.unlock();

            std::vector<lsp::Diagnostic> diagnostics;
            if (uri.find("as.predefined") != std::string::npos || uri.ends_with(".predefined"))
            {
                diagnostics = {};
            }
            else
            {
                std::function<const Document *(const std::string &)> docResolver = [this](const std::string &u) -> const Document * {
                    std::shared_lock docLock(m_docMutex);
                    std::string normU = NormalizeUri(u);
                    auto it = m_documents.find(normU);
                    if (it != m_documents.end())
                        return it->second.get();
                    return nullptr;
                };

                std::lock_guard<std::mutex> engineLock(m_engineMutex);
                diagnostics = oracle->ValidateSync(text, uri, docResolver);
            }

            m_diagCache->Update(uri, diagnostics);

            lsp::notifications::TextDocument_PublishDiagnostics::Params params;
            params.uri = lsp::DocumentUri::parse(uri);
            params.diagnostics = std::move(diagnostics);

            messageHandler->sendNotification<lsp::notifications::TextDocument_PublishDiagnostics>(std::move(params));
        }
    }

} // namespace angel_lsp
