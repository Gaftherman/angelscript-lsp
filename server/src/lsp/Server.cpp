#include "Server.h"
#include "utils/Utils.h"

#include <filesystem>
#include <fstream>
#include <variant>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
    Server::Server(const angel_lsp::config::ServerConfig &config)
    {
        m_config = config;

        m_connection = std::make_unique<lsp::Connection>(lsp::io::standardIO());
        m_messageHandler = std::make_unique<lsp::MessageHandler>(*m_connection);

        m_running = true;

        m_logger = std::make_unique<angel_lsp::utils::LspLogger>(m_messageHandler.get());

        m_parser = std::make_unique<angel_lsp::parser::AngelScriptParser>(m_logger.get());

        m_symbolCollector = std::make_unique<angel_lsp::analysis::SymbolCollector>(m_logger.get());
        m_semanticAnalyzer = std::make_unique<angel_lsp::analysis::SemanticAnalyzer>(m_logger.get());

        InitHandles();
    }

    Server::~Server()
    {
        m_running = false;
    }

    void Server::Run()
    {
        while (m_running)
        {
            m_messageHandler->processIncomingMessages();
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

        if (params.locale.has_value())
        {
            m_locale = params.locale.value();
        }

        lsp::requests::Initialize::Result result;

        lsp::InitializeResultServerInfo info;
        info.name = m_config.info.name;
        info.version = m_config.info.version;
        result.serverInfo = info;

        lsp::TextDocumentSyncOptions sync;
        sync.openClose = true;
        sync.change = lsp::TextDocumentSyncKind::Incremental;

        lsp::SaveOptions saveOptions;
        saveOptions.includeText = true;
        sync.save = saveOptions;

        result.capabilities.textDocumentSync = sync;

        return result;
    }

    void Server::HandleNotificationsInitialized(lsp::notifications::Initialized::Params &&params)
    {
        m_workspaceThread = std::jthread([this](std::stop_token stopToken)
                                         { this->ReadWorkspaceFiles(stopToken); });
    }

    void Server::ReadWorkspaceFiles(std::stop_token stopToken)
    {
        angel_lsp::parser::AngelScriptParser backgroundParser(m_logger.get());

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

    void Server::ParserPredefined(const std::string &filePath, angel_lsp::parser::AngelScriptParser &parser)
    {
        std::string uri = angel_lsp::utils::PathToUri(filePath);

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            m_logger->LogError(fmt::format("Cannot open predefined file: {}", filePath));
            return;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        m_symbolTable.ClearDocumentSymbols(uri);
        m_symbolCollector->CollectSymbols(uri, content, parser, m_symbolTable);

        {
            std::lock_guard<std::mutex> lock(m_predefinedMutex);
            m_predefinedUris.insert(uri);
        }

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
        if (params.settings.isString())
        {
            m_logger->LogInfo(fmt::format("Workspace configuration changed: {}", params.settings.string()));
        }
    }

    void Server::HandleNotificationsTextDocument_DidSave(lsp::notifications::TextDocument_DidSave::Params &&params)
    {
        std::string uriStr = params.textDocument.uri.toString();
        std::string text = params.text.has_value() ? params.text.value() : "";

        if (text.empty() && m_openDocuments.contains(uriStr))
            text = m_openDocuments[uriStr];

        m_symbolTable.ClearDocumentSymbols(uriStr);

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            m_symbolCollector->CollectSymbols(uriStr, text, *m_parser, m_symbolTable);
            PublishDiagnostics(uriStr, {});
            return;
        }

        auto diagnostics = m_symbolCollector->CollectSymbols(uriStr, text, *m_parser, m_symbolTable);

        angel_lsp::analysis::SemanticAnalysisRequest req{m_symbolTable, uriStr, std::string(m_config.info.predefinedFileExtension)};
        auto semanticDiagnostics = m_semanticAnalyzer->Analyze(req);
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

        PublishDiagnostics(uriStr, diagnostics);
    }

    void Server::HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params &&params)
    {
        std::string uriStr = params.textDocument.uri.toString();
        std::string text = params.textDocument.text;

        m_openDocuments[uriStr] = text;

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            {
                std::lock_guard<std::mutex> lock(m_predefinedMutex);
                if (!m_predefinedUris.contains(uriStr))
                {
                    m_symbolTable.ClearDocumentSymbols(uriStr);
                    m_symbolCollector->CollectSymbols(uriStr, text, *m_parser, m_symbolTable);
                    m_predefinedUris.insert(uriStr);
                }
            }
            PublishDiagnostics(uriStr, {});
            return;
        }

        m_symbolTable.ClearDocumentSymbols(uriStr);
        auto diagnostics = m_symbolCollector->CollectSymbols(uriStr, text, *m_parser, m_symbolTable);

        angel_lsp::analysis::SemanticAnalysisRequest req{m_symbolTable, uriStr, std::string(m_config.info.predefinedFileExtension)};
        auto semanticDiagnostics = m_semanticAnalyzer->Analyze(req);
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

        PublishDiagnostics(uriStr, diagnostics);
    }

    void Server::HandleNotificationsTextDocument_DidChange(lsp::notifications::TextDocument_DidChange::Params &&params)
    {
        std::string uriStr = params.textDocument.uri.toString();
        auto it = m_openDocuments.find(uriStr);
        if (it == m_openDocuments.end())
            return;

        std::string &buffer = it->second;

        for (const auto &change : params.contentChanges)
        {
            if (std::holds_alternative<lsp::TextDocumentContentChangeEvent_Range_Text>(change))
            {
                const auto &rt = std::get<lsp::TextDocumentContentChangeEvent_Range_Text>(change);
                angel_lsp::utils::ApplyIncrementalChange(buffer,
                                                         rt.range.start.line, rt.range.start.character,
                                                         rt.range.end.line, rt.range.end.character,
                                                         rt.text);
            }
            else if (std::holds_alternative<lsp::TextDocumentContentChangeEvent_Text>(change))
            {
                const auto &t = std::get<lsp::TextDocumentContentChangeEvent_Text>(change);
                buffer = t.text;
            }
        }

        m_symbolTable.ClearDocumentSymbols(uriStr);

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            m_symbolCollector->CollectSymbols(uriStr, buffer, *m_parser, m_symbolTable);
            PublishDiagnostics(uriStr, {});
            return;
        }

        auto diagnostics = m_symbolCollector->CollectSymbols(uriStr, buffer, *m_parser, m_symbolTable);

        angel_lsp::analysis::SemanticAnalysisRequest req{m_symbolTable, uriStr, std::string(m_config.info.predefinedFileExtension)};
        auto semanticDiagnostics = m_semanticAnalyzer->Analyze(req);
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

        PublishDiagnostics(uriStr, diagnostics);
    }

    void Server::HandleNotificationsTextDocument_DidClose(lsp::notifications::TextDocument_DidClose::Params &&params)
    {
        std::string uriStr = params.textDocument.uri.toString();
        m_openDocuments.erase(uriStr);

        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            PublishDiagnostics(uriStr, {});
            return;
        }

        m_symbolTable.ClearDocumentSymbols(uriStr);
        PublishDiagnostics(uriStr, {});
    }

    void Server::PublishDiagnostics(const std::string &uriStr, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics)
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
            lspDiag.severity = static_cast<lsp::DiagnosticSeverity>(diag.severity);
            lspDiag.source = diag.source;
            lspDiag.code = diag.code;

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
    }
}