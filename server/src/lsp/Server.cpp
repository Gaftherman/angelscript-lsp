#include "Server.h"
#include "utils/Utils.h"

#include <filesystem>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
    Server::Server(const angel_lsp::config::ServerConfig &config)
    {
        // Config
        m_config = config;

        // Initialize the connection and message handler
        m_connection = std::make_unique<lsp::Connection>(lsp::io::standardIO());
        m_messageHandler = std::make_unique<lsp::MessageHandler>(*m_connection);

        // Set the running flag to true
        m_running = true;

        // Initialize the logger
        m_logger = std::make_unique<angel_lsp::utils::LspLogger>(m_messageHandler.get());

        // Initialize the parser
        m_parser = std::make_unique<angel_lsp::parser::AngelScriptParser>(m_logger.get());

        // Initialize the symbol collector
        m_symbolCollector = std::make_unique<angel_lsp::analysis::SymbolCollector>(m_logger.get());

        // Initialize the semantic analyzer
        m_semanticAnalyzer = std::make_unique<angel_lsp::analysis::SemanticAnalyzer>(m_logger.get());

        // Initialize message handlers
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
        // Store workspace folders if available
        if (params.workspaceFolders.has_value() && !params.workspaceFolders.value().isNull())
        {
            for (const auto &workspace : params.workspaceFolders.value().value())
            {
                m_workspacesRoot.push_back(std::string(workspace.uri.path()));
            }
        }

        // Store the locale
        if (params.locale.has_value())
        {
            m_locale = params.locale.value();
        }

        // Prepare the result for the Initialize request
        lsp::requests::Initialize::Result result;

        lsp::InitializeResultServerInfo info;
        info.name = m_config.info.name;
        info.version = m_config.info.version;
        result.serverInfo = info;

        lsp::SaveOptions saveOptions;
        saveOptions.includeText = true;

        lsp::TextDocumentSyncOptions sync;
        sync.openClose = true;
        sync.change = lsp::TextDocumentSyncKind::Incremental;
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
                        // Check if the file is a predefined file (e.g., ends with ".as.predefined" or matches the predefined file extension)
                        if (entry.path().string().ends_with(m_config.info.predefinedFileExtension) || entry.path().string() == m_config.info.predefinedFileExtension)
                            ParserPredefined(entry.path().string());

                        // For future, save all .as files to a list for parsing and indexing for the #include directive
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            m_logger->LogError(fmt::format("Error reading workspace files: {}", e.what()));
        }
    }

    void Server::ParserPredefined(const std::string &filePath)
    {
        m_logger->LogInfo(fmt::format("Parsing predefined file: {}", filePath));
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
        std::string text = params.text.has_value() ? params.text.value() : /*ReadFileContent(angel_lsp::utils::UriToPath(uriStr))*/ "";

        // m_symbolCollector->CollectSymbols(uriStr, text, *m_parser, m_symbolTable);

        // !TODO Incremental collector symbol update for the saved document (if needed)
        m_logger->LogInfo(fmt::format("Texto guardado: {}", text));
    }

    void Server::HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params &&params)
    {
        std::string uriStr = params.textDocument.uri.toString();
        std::string text = params.textDocument.text;

        // Clear existing symbols for the document and collect new symbols
        m_symbolTable.ClearDocumentSymbols(uriStr);

        // Collect symbols from the opened document
        m_symbolCollector->CollectSymbols(uriStr, text, *m_parser, m_symbolTable);

        // Perform semantic analysis on the collected symbols for the document
        angel_lsp::analysis::SemanticAnalysisRequest request{m_symbolTable, uriStr};
        std::vector<angel_lsp::analysis::Diagnostic> diagnostics = m_semanticAnalyzer->Analyze(request);

        // Publish diagnostics to the client
        m_logger->LogInfo(fmt::format("Diagnósticos para {}: {}", uriStr, diagnostics.size()));
    }

    void Server::HandleNotificationsTextDocument_DidChange(lsp::notifications::TextDocument_DidChange::Params &&params)
    {
    }

    void Server::HandleNotificationsTextDocument_DidClose(lsp::notifications::TextDocument_DidClose::Params &&params)
    {
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