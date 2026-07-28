#include "LspLogger.h"

namespace angel_lsp::utils
{
    LspLogger::LspLogger(lsp::MessageHandler *messageHandler)
        : m_messageHandler(messageHandler)
    {
    }

    void LspLogger::LogWarning(std::string_view message)
    {
        std::lock_guard<std::mutex> lock(m_logMutex);

        lsp::notifications::Window_LogMessage::Params logParams;
        logParams.type = lsp::MessageType::Warning;
        logParams.message = message;
        m_messageHandler->sendNotification<lsp::notifications::Window_LogMessage>(std::move(logParams));
    }

    void LspLogger::LogInfo(std::string_view message)
    {
        std::lock_guard<std::mutex> lock(m_logMutex);

        lsp::notifications::Window_LogMessage::Params logParams;
        logParams.type = lsp::MessageType::Info;
        logParams.message = message;
        m_messageHandler->sendNotification<lsp::notifications::Window_LogMessage>(std::move(logParams));
    }

    void LspLogger::LogError(std::string_view message)
    {
        std::lock_guard<std::mutex> lock(m_logMutex);

        lsp::notifications::Window_LogMessage::Params logParams;
        logParams.type = lsp::MessageType::Error;
        logParams.message = message;
        m_messageHandler->sendNotification<lsp::notifications::Window_LogMessage>(std::move(logParams));
    }

    void LspLogger::LogDebug(std::string_view message)
    {
        std::lock_guard<std::mutex> lock(m_logMutex);

        lsp::notifications::Window_LogMessage::Params logParams;
        logParams.type = lsp::MessageType::Debug;
        logParams.message = message;
        m_messageHandler->sendNotification<lsp::notifications::Window_LogMessage>(std::move(logParams));
    }
}