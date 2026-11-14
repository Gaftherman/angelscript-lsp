#include "lsp/mcp/LspClientSession.h"

#include "lsp/Server.h"
#include "utils/MultiFileLogger.h"

#include <chrono>
#include <cstdlib>
#include <stdexcept>

namespace angel_lsp::mcp
{

/// Largest message body accepted from the server; matches the transport bound of the protocol layer.
static constexpr std::size_t k_maxBodyBytes = 16u * 1024u * 1024u;
/// Longest a tool call waits for one reply before reporting a failure to the MCP caller.
static constexpr std::chrono::seconds k_replyTimeout{120};

static lsp::json::Value MakeObject()
{
    return lsp::json::Value(lsp::json::Object());
}

LspClientSession::LspClientSession(const config::ServerConfig& config, std::string_view rootUri)
{
    config::ServerConfig inner = config;
    inner.info.mcpMode = false;
    inner.features.enablePullDiagnostics = true;

    m_serverThread = std::thread(
        [this, inner]
        {
            try
            {
                Server server(inner, m_pipe);
                server.Run();
            }
            catch (const std::exception& e)
            {
                utils::MultiFileLogger::Instance().LogCrash(std::string("mcp server thread: ") + e.what());
            }
            m_pipe.Close();
        });
    m_readerThread = std::thread(
        [this]
        {
            try
            {
                ReaderLoop();
            }
            catch (const std::exception& e)
            {
                utils::MultiFileLogger::Instance().LogCrash(std::string("mcp reader thread: ") + e.what());
            }
            FailAllPending("server connection closed");
        });

    lsp::json::Value params = MakeObject();
    params.object()["processId"] = nullptr;
    lsp::json::Value caps = MakeObject();
    lsp::json::Value textDocument = MakeObject();
    textDocument.object()["diagnostic"] = MakeObject();
    caps.object()["textDocument"] = std::move(textDocument);
    params.object()["capabilities"] = std::move(caps);
    if (!rootUri.empty())
    {
        params.object()["rootUri"] = std::string(rootUri);
        lsp::json::Array folders;
        lsp::json::Value folder = MakeObject();
        folder.object()["uri"] = std::string(rootUri);
        folder.object()["name"] = std::string("workspace");
        folders.push_back(std::move(folder));
        params.object()["workspaceFolders"] = std::move(folders);
    }
    Request("initialize", std::move(params));
    Notify("initialized", MakeObject());
}

LspClientSession::~LspClientSession()
{
    try
    {
        Request("shutdown", nullptr);
        Notify("exit", nullptr);
    }
    catch (const std::exception&)
    {
        // The server is already gone; closing the pipe below is all that is left to do.
    }
    m_pipe.Close();
    if (m_serverThread.joinable())
    {
        m_serverThread.join();
    }
    if (m_readerThread.joinable())
    {
        m_readerThread.join();
    }
}

void LspClientSession::Send(const lsp::json::Value& message)
{
    const std::string body = lsp::json::stringify(message);
    m_pipe.ClientWrite("Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
}

lsp::json::Value LspClientSession::Request(std::string_view method, lsp::json::Value params)
{
    const int id = m_nextId.fetch_add(1);
    std::future<lsp::json::Value> reply;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_readerDone)
        {
            throw std::runtime_error("server connection closed");
        }
        reply = m_pending[id].get_future();
    }

    lsp::json::Value message = MakeObject();
    message.object()["jsonrpc"] = std::string("2.0");
    message.object()["id"] = id;
    message.object()["method"] = std::string(method);
    if (!params.isNull())
    {
        message.object()["params"] = std::move(params);
    }
    Send(message);

    if (reply.wait_for(k_replyTimeout) != std::future_status::ready)
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.erase(id);
        throw std::runtime_error("timed out waiting for " + std::string(method));
    }
    return reply.get();
}

void LspClientSession::Notify(std::string_view method, lsp::json::Value params)
{
    lsp::json::Value message = MakeObject();
    message.object()["jsonrpc"] = std::string("2.0");
    message.object()["method"] = std::string(method);
    if (!params.isNull())
    {
        message.object()["params"] = std::move(params);
    }
    Send(message);
}

bool LspClientSession::ReadMessage(lsp::json::Value& out)
{
    std::string header;
    std::string one;
    while (header.size() < 8192)
    {
        if (!m_pipe.ClientRead(one, 1))
        {
            return false;
        }
        header += one;
        if (header.size() >= 4 && header.compare(header.size() - 4, 4, "\r\n\r\n") == 0)
        {
            break;
        }
    }

    static constexpr std::string_view k_lengthKey = "Content-Length:";
    const std::size_t keyPos = header.find(k_lengthKey);
    if (keyPos == std::string::npos)
    {
        return false;
    }
    const unsigned long long length = std::strtoull(header.c_str() + keyPos + k_lengthKey.size(), nullptr, 10);
    if (length == 0 || length > k_maxBodyBytes)
    {
        return false;
    }

    std::string body;
    if (!m_pipe.ClientRead(body, static_cast<std::size_t>(length)))
    {
        return false;
    }
    out = lsp::json::parse(body);
    return true;
}

void LspClientSession::ReaderLoop()
{
    lsp::json::Value message;
    while (ReadMessage(message))
    {
        Dispatch(std::move(message));
    }
}

void LspClientSession::Dispatch(lsp::json::Value&& message)
{
    if (!message.isObject())
    {
        return;
    }
    const lsp::json::Object& obj = message.object();
    const lsp::json::Value* id = obj.find("id");
    const bool hasMethod = obj.contains("method");

    if (hasMethod && id != nullptr)
    {
        lsp::json::Value ack = MakeObject();
        ack.object()["jsonrpc"] = std::string("2.0");
        ack.object()["id"] = *id;
        ack.object()["result"] = nullptr;
        Send(ack);
        return;
    }
    if (hasMethod || id == nullptr || !id->isInteger())
    {
        return;
    }

    std::promise<lsp::json::Value> promise;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        const auto it = m_pending.find(id->integer());
        if (it == m_pending.end())
        {
            return;
        }
        promise = std::move(it->second);
        m_pending.erase(it);
    }

    if (const lsp::json::Value* error = obj.find("error"))
    {
        std::string text = "LSP error";
        int code = 0;
        if (error->isObject() && error->object().contains("message") && error->object().get("message").isString())
        {
            text = error->object().get("message").string();
        }
        if (error->isObject() && error->object().contains("code") && error->object().get("code").isInteger())
        {
            code = error->object().get("code").integer();
        }
        promise.set_exception(std::make_exception_ptr(LspRequestError(code, text)));
        return;
    }
    const lsp::json::Value* result = obj.find("result");
    promise.set_value(result != nullptr ? *result : lsp::json::Value());
}

void LspClientSession::FailAllPending(std::string_view reason)
{
    std::map<int, std::promise<lsp::json::Value>> pending;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_readerDone = true;
        pending.swap(m_pending);
    }
    for (auto& entry : pending)
    {
        entry.second.set_exception(std::make_exception_ptr(std::runtime_error(std::string(reason))));
    }
}

} // namespace angel_lsp::mcp
