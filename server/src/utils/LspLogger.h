#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <memory>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

namespace angel_lsp::utils
{
    /**
     * @brief Severity threshold for LspLogger. Ordered least to most verbose.
     */
    enum class LogLevel
    {
        Error = 0,
        Warning = 1,
        Info = 2,
        Debug = 3
    };

    /**
     * @brief Sends log records to the client as window/logMessage notifications.
     *
     * @note Every call that passes the level check writes a real JSON-RPC frame down the same
     *       connection the responses go through, so it is not free the way a local log file would
     *       be. That is why the threshold exists and why anything expensive to format must be
     *       guarded with IsEnabled() rather than merely passed to LogDebug() - see the symbol dump
     *       in SemanticAnalyzer::Analyze, which used to format one message per symbol per keystroke
     *       whether or not anyone was listening.
     */
    class LspLogger
    {
    private:
        lsp::MessageHandler *m_messageHandler;
        std::mutex m_logMutex;

        // Atomic rather than mutex-guarded: it is read on the message loop, the analysis thread and
        // the workspace thread, and read far more often than it is written (once, at startup).
        std::atomic<LogLevel> m_level{ LogLevel::Info };

    public:
        LspLogger(lsp::MessageHandler *messageHandler);

        /** @brief Sets the threshold. Records below it are dropped before being formatted. */
        void SetLevel(LogLevel level) noexcept { m_level.store(level, std::memory_order_relaxed); }

        LogLevel GetLevel() const noexcept { return m_level.load(std::memory_order_relaxed); }

        /**
         * @brief True when a record at this level would actually be sent.
         *
         * Call this before building an expensive message. Passing an already-formatted string to
         * LogDebug() still pays for the formatting even when the record is dropped.
         */
        bool IsEnabled(LogLevel level) const noexcept
        {
            return static_cast<int>(level) <= static_cast<int>(m_level.load(std::memory_order_relaxed));
        }

        /** @brief Shorthand for the check that guards the hot paths. */
        bool IsDebugEnabled() const noexcept { return IsEnabled(LogLevel::Debug); }

        void LogWarning(std::string_view message);
        void LogInfo(std::string_view message);
        void LogError(std::string_view message);
        void LogDebug(std::string_view message);
    };

    /**
     * @brief Parses a level name (`error`, `warning`, `info`, `debug`), case-insensitively.
     * @return The parsed level, or fallback when the name is not recognised.
     */
    LogLevel ParseLogLevel(std::string_view name, LogLevel fallback = LogLevel::Info);
}
