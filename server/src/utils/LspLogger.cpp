#include "LspLogger.h"

#include <algorithm>
#include <cctype>

namespace angel_lsp::utils
{
    namespace
    {
        /** @brief Sends one record, or does nothing when the level is below the threshold. */
        void Send(lsp::MessageHandler *handler, lsp::MessageType type, std::string_view message)
        {
            lsp::notifications::Window_LogMessage::Params logParams;
            logParams.type = type;
            logParams.message = message;
            handler->sendNotification<lsp::notifications::Window_LogMessage>(std::move(logParams));
        }
    }

    LspLogger::LspLogger(lsp::MessageHandler *messageHandler)
        : m_messageHandler(messageHandler)
    {
    }

    void LspLogger::LogError(std::string_view message)
    {
        if (!IsEnabled(LogLevel::Error))
            return;

        std::lock_guard<std::mutex> lock(m_logMutex);
        Send(m_messageHandler, lsp::MessageType::Error, message);
    }

    void LspLogger::LogWarning(std::string_view message)
    {
        if (!IsEnabled(LogLevel::Warning))
            return;

        std::lock_guard<std::mutex> lock(m_logMutex);
        Send(m_messageHandler, lsp::MessageType::Warning, message);
    }

    void LspLogger::LogInfo(std::string_view message)
    {
        if (!IsEnabled(LogLevel::Info))
            return;

        std::lock_guard<std::mutex> lock(m_logMutex);
        Send(m_messageHandler, lsp::MessageType::Info, message);
    }

    void LspLogger::LogDebug(std::string_view message)
    {
        if (!IsEnabled(LogLevel::Debug))
            return;

        std::lock_guard<std::mutex> lock(m_logMutex);
        Send(m_messageHandler, lsp::MessageType::Debug, message);
    }

    LogLevel ParseLogLevel(std::string_view name, LogLevel fallback)
    {
        std::string lowered;
        lowered.reserve(name.size());
        for (const char c : name)
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        if (lowered == "error")   return LogLevel::Error;
        if (lowered == "warning" || lowered == "warn") return LogLevel::Warning;
        if (lowered == "info")    return LogLevel::Info;
        if (lowered == "debug" || lowered == "verbose") return LogLevel::Debug;

        return fallback;
    }
}
