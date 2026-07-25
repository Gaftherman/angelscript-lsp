#pragma once

#include "config/ServerConfig.h"
#include "utils/LspLogger.h"
#include "parser/AngelScriptParser.h"

#include <lsp/messages.h>
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/messagehandler.h>

#include <string>
#include <vector>
#include <thread>
#include <mutex>

namespace angel_lsp
{
    class Server
    {
    private:
        angel_lsp::config::ServerConfig m_config;
        std::unique_ptr<lsp::Connection> m_connection;
        std::unique_ptr<lsp::MessageHandler> m_messageHandler;
        bool m_running;
        std::vector<std::string> m_workspacesRoot;
        std::string m_locale;
        std::jthread m_workspaceThread;
        std::mutex m_messageHandlerMutex;
        std::unique_ptr<angel_lsp::utils::LspLogger> m_logger;
        std::unique_ptr<angel_lsp::parser::AngelScriptParser> m_parser;

    public:
        Server(const angel_lsp::config::ServerConfig &config);
        ~Server();

        void Run();
        void InitHandles();

        auto HandleRequestsInitialized(lsp::requests::Initialize::Params &&params);
        void HandleNotificationsInitialized(lsp::notifications::Initialized::Params &&params);
        auto HandleRequestsShutdown();
        void HandleNotificationsExit();
        void HandleNotificationsWorkspace_DidChangeConfiguration(lsp::notifications::Workspace_DidChangeConfiguration::Params &&params);
        void HandleNotificationsTextDocument_DidSave(lsp::notifications::TextDocument_DidSave::Params &&params);
        void HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params &&params);
        void HandleNotificationsTextDocument_DidChange(lsp::notifications::TextDocument_DidChange::Params &&params);
        void HandleNotificationsTextDocument_DidClose(lsp::notifications::TextDocument_DidClose::Params &&params);
        void ReadWorkspaceFiles(std::stop_token stopToken);
        void ParserPredefined(const std::string &filePath);
    };
}