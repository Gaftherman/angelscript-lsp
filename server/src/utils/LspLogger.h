/**
 * @file LspLogger.h
 * @brief Thread-safe LSP window/logMessage bridge utility.
 * @ingroup Utils
 */

#pragma once

#include <string>
#include <memory>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

namespace angel_lsp
{
    /**
     * @brief Global logger utility that bridges to the LSP framework.
     * @note Thread-safe logger interface avoiding stdout/stderr stream corruption.
     */
    class LspLogger
    {
    public:
        /**
         * @brief Initializes the global logger with the active message handler.
         *
         * @param[in] handler Pointer to the active LSP message handler.
         * @note Thread-safe initialization method.
         */
        static void Initialize(lsp::MessageHandler *handler);

        /**
         * @brief Logs an informational message to the LSP client via window/logMessage.
         *
         * @param[in] msg The log message string.
         * @note Thread-safe logging method.
         */
        static void Info(const std::string &msg);

        /**
         * @brief Logs a warning message to the LSP client via window/logMessage.
         *
         * @param[in] msg The log message string.
         * @note Thread-safe logging method.
         */
        static void Warn(const std::string &msg);

        /**
         * @brief Logs an error message to the LSP client via window/logMessage.
         *
         * @param[in] msg The log message string.
         * @note Thread-safe logging method.
         */
        static void Error(const std::string &msg);

    private:
        static lsp::MessageHandler *s_handler;
    };
}
