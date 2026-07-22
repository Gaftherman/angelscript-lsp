/**
 * @file Server.h
 * @brief Top-level Language Server Protocol orchestrator, request dispatcher, and validation worker.
 * @ingroup Server
 */

#pragma once

#include <iostream>
#include <unordered_map>
#include <string>
#include <memory>
#include "document/Document.h"
#include "analysis/ValidationOracle.h"
#include "analysis/SymbolTable.h"
#include "analysis/DiagnosticCache.h"
#include "config/ServerConfig.h"
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <ankerl/unordered_dense.h>
#include "i18n/LspStrings.h"

namespace lsp
{
    class MessageHandler;
    class Connection;
}

namespace angel_lsp
{

    /**
     * @brief The main LSP Server class orchestrating all language intelligence and JSON-RPC messaging.
     * @note Thread-safe orchestrator using std::shared_mutex for document reads and std::jthread background validation worker.
     */
    class Server
    {
    public:
        /**
         * @brief Constructs the LSP server with an optional configuration.
         *
         * @param[in] config Configuration settings and feature flags.
         */
        explicit Server(ServerConfig config = ServerConfig());

        /**
         * @brief Destroys the LSP server and cleans up resources.
         */
        ~Server();

        /**
         * @brief Starts the main LSP server execution loop.
         */
        void Run();

    private:
        /**
         * @brief Registers all LSP request and notification handlers.
         */
        void RegisterHandlers();

        /**
         * @brief Schedules background document diagnostics validation.
         *
         * @param[in] uri Document URI string.
         * @param[in] text Document source text.
         */
        void ScheduleValidation(std::string uri, std::string text);

        /**
         * @brief Worker thread loop for processing background diagnostics with 300ms debounce.
         *
         * @param[in] st Thread stop token.
         */
        void ValidationWorkerLoop(std::stop_token st);

        ServerConfig m_config;

        std::unique_ptr<::lsp::Connection> m_connection;
        std::unique_ptr<::lsp::MessageHandler> messageHandler;
        std::shared_mutex m_docMutex;
        ankerl::unordered_dense::map<std::string, std::unique_ptr<Document>> m_documents;
        ankerl::unordered_dense::map<std::string, analysis::SymbolTable> m_symbolTables;
        analysis::SymbolTable m_globalSymbolTable;
        std::unique_ptr<analysis::DiagnosticCache> m_diagCache;

        std::unique_ptr<analysis::ValidationOracle> oracle;
        bool running = true;

        std::jthread m_validationThread;
        std::mutex m_validationMutex;
        std::condition_variable_any m_validationCV;
        bool m_validationPending = false;
        std::string m_pendingUri;
        std::string m_pendingText;

        std::jthread m_predefinedThread;

        i18n::Locale m_locale = i18n::Locale::EN;
        std::string m_workspaceRoot;
    };

} // namespace angel_lsp
