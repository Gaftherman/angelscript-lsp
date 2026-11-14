#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <lsp/io/stream.h>
#include <mutex>
#include <string>
#include <string_view>

namespace angel_lsp::mcp
{

/**
 * @brief Bidirectional in-memory transport between an in-process Server and an in-process client.
 *
 * The server side is the lsp::io::Stream interface; the client side is ClientWrite and ClientRead.
 * @note Close wakes every blocked reader; the server side then sees lsp::io::Error and Server::Run returns.
 * @note Thread-safe: one mutex and condition variable guard both directions.
 */
class PipeStream final : public lsp::io::Stream
{
  public:
    /** @brief Server side: blocks until size bytes of client input are available; throws lsp::io::Error once closed. */
    void read(char* buffer, std::size_t size) override;

    /** @brief Server side: queues bytes for the client; a closed pipe drops them. */
    void write(const char* buffer, std::size_t size) override;

    /** @brief Client side: queues bytes for the server. */
    void ClientWrite(std::string_view bytes);

    /**
     * @brief Client side: blocks until size bytes of server output are available.
     * @param[out] out Receives exactly size bytes on success.
     * @param[in] size Number of bytes to take.
     * @return False when the pipe closed before size bytes arrived.
     */
    bool ClientRead(std::string& out, std::size_t size);

    /** @brief Wakes every blocked reader and makes later reads fail. Idempotent. */
    void Close();

  private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<char> m_toServer;
    std::deque<char> m_toClient;
    bool m_closed = false;
};

} // namespace angel_lsp::mcp
