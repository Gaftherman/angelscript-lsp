#pragma once

#include "i18n/i18n.h"
#include "config/ServerConfig.h"
#include "utils/LspLogger.h"
#include "parser/AngelScriptParser.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SemanticAnalyzer.h"

#include <lsp/messages.h>
#include <lsp/connection.h>
#include <lsp/io/standardio.h>
#include <lsp/messagehandler.h>
#include <ankerl/unordered_dense.h>

#include <tree_sitter/api.h>
#include <unordered_map>
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
        std::unique_ptr<angel_lsp::i18n::I18n> m_i18n;
        std::jthread m_workspaceThread;
        std::mutex m_messageHandlerMutex;
        std::unique_ptr<angel_lsp::utils::LspLogger> m_logger;
        std::unique_ptr<angel_lsp::parser::AngelScriptParser> m_parser;
        angel_lsp::analysis::SymbolTable m_symbolTable;
        std::unique_ptr<angel_lsp::analysis::SymbolCollector> m_symbolCollector;
        std::unique_ptr<angel_lsp::analysis::SemanticAnalyzer> m_semanticAnalyzer;
        ankerl::unordered_dense::map<std::string, std::string> m_openDocuments;
        std::unordered_map<std::string, TSTree*> m_documentTrees;
        std::mutex m_predefinedMutex;
        ankerl::unordered_dense::set<std::string> m_predefinedUris;

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
        void ParserPredefined(const std::string &filePath, angel_lsp::parser::AngelScriptParser &parser);
        void PublishDiagnostics(const std::string &uriStr, const std::vector<angel_lsp::analysis::Diagnostic> &diagnostics);
    };
}