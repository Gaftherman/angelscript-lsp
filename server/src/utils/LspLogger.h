#pragma once

#include <string>
#include <memory>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

namespace angel_lsp::utils
{
    class LspLogger
    {
    private:
        lsp::MessageHandler *m_messageHandler;
        std::mutex m_logMutex;

    public:
        LspLogger(lsp::MessageHandler *messageHandler);
        void LogWarning(std::string_view message);
        void LogInfo(std::string_view message);
        void LogError(std::string_view message);
        void LogDebug(std::string_view message);
    };
}