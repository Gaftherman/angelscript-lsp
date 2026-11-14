#pragma once

#include "config/ServerConfig.h"
#include "lsp/mcp/PipeStream.h"

#include <atomic>
#include <future>
#include <lsp/json/json.h>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace angel_lsp::mcp
{

/**
 * @brief An LSP client that lives in the same process as the Server it drives.
 *
 * Starts a Server thread over a PipeStream, completes the initialize handshake and offers synchronous
 * Request and Notify calls.
 * @note Requests the server sends to the client are answered with null so the server never blocks.
 * @note Request and Notify are for one calling thread at a time; replies are matched by id.
 */
class LspClientSession
{
  public:
    /**
     * @brief Starts the server thread and completes the initialize handshake.
     * @param[in] config Server configuration; MCP mode is cleared for the inner server.
     * @param[in] rootUri Workspace root URI sent as rootUri and workspaceFolders; empty for none.
     */
    LspClientSession(const config::ServerConfig& config, std::string_view rootUri);

    /** @brief Sends shutdown and exit, closes the pipe and joins both threads. */
    ~LspClientSession();

    LspClientSession(const LspClientSession&) = delete;
    LspClientSession& operator=(const LspClientSession&) = delete;

    /**
     * @brief Sends a request and waits for its reply.
     * @param[in] method LSP method name.
     * @param[in] params Request params; null sends none.
     * @return The result member of the reply.
     * @throws std::runtime_error on an error reply, a closed pipe or a missing reply within the limit.
     */
    lsp::json::Value Request(std::string_view method, lsp::json::Value params);

    /** @brief Sends a notification. */
    void Notify(std::string_view method, lsp::json::Value params);

  private:
    void Send(const lsp::json::Value& message);
    void ReaderLoop();
    bool ReadMessage(lsp::json::Value& out);
    void Dispatch(lsp::json::Value&& message);
    void FailAllPending(std::string_view reason);

    PipeStream m_pipe;
    std::thread m_serverThread;
    std::thread m_readerThread;
    std::atomic<int> m_nextId{1};
    std::mutex m_pendingMutex;
    std::map<int, std::promise<lsp::json::Value>> m_pending;
    bool m_readerDone = false;
};

} // namespace angel_lsp::mcp
