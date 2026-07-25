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
        void LogInfo(std::string_view message);
        void LogWarning(std::string_view message);
        void LogError(std::string_view message);
    };
}