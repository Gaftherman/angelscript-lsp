#pragma once
#include <string>
#include <memory>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

namespace angel_lsp
{
    /**
     * @brief A global logger that bridges to the LSP framework.
     * 
     * This singleton allows any class (like QueryRunner or ValidationOracle) 
     * to send log messages natively through the LSP protocol (window/logMessage),
     * avoiding stdout/stderr corruption.
     */
    class LspLogger
    {
    public:
        /**
         * @brief Initializes the global logger with the active message handler.
         * 
         * @param handler A pointer to the LSP message handler.
         */
        static void Initialize(lsp::MessageHandler *handler);

        /**
         * @brief Logs an informational message to the LSP client.
         * 
         * @param msg The message to log.
         */
        static void Info(const std::string &msg);

        /**
         * @brief Logs a warning message to the LSP client.
         * 
         * @param msg The message to log.
         */
        static void Warn(const std::string &msg);

        /**
         * @brief Logs an error message to the LSP client.
         * 
         * @param msg The message to log.
         */
        static void Error(const std::string &msg);

    private:
        static lsp::MessageHandler *s_handler;
    };
}
