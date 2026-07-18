#include "LspLogger.h"

namespace angel_lsp
{
    lsp::MessageHandler *LspLogger::s_handler = nullptr;

    void LspLogger::Initialize(lsp::MessageHandler *handler)
    {
        s_handler = handler;
    }

    void LspLogger::Info(const std::string &msg)
    {
        if (!s_handler)
            return;

        lsp::notifications::Window_LogMessage::Params p;
        p.type = lsp::MessageType::Info;
        p.message = msg;
        s_handler->sendNotification<lsp::notifications::Window_LogMessage>(std::move(p));
    }

    void LspLogger::Warn(const std::string &msg)
    {
        if (!s_handler)
            return;

        lsp::notifications::Window_LogMessage::Params p;
        p.type = lsp::MessageType::Warning;
        p.message = msg;
        s_handler->sendNotification<lsp::notifications::Window_LogMessage>(std::move(p));
    }

    void LspLogger::Error(const std::string &msg)
    {
        if (!s_handler)
            return;

        lsp::notifications::Window_LogMessage::Params p;
        p.type = lsp::MessageType::Error;
        p.message = msg;
        s_handler->sendNotification<lsp::notifications::Window_LogMessage>(std::move(p));
    }
}
