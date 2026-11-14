#include "lsp/mcp/PipeStream.h"

namespace angel_lsp::mcp
{

void PipeStream::read(char* buffer, std::size_t size)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this, size] { return m_closed || m_toServer.size() >= size; });
    if (m_toServer.size() < size)
    {
        throw lsp::io::Error("mcp pipe closed");
    }
    for (std::size_t i = 0; i < size; ++i)
    {
        buffer[i] = m_toServer.front();
        m_toServer.pop_front();
    }
}

void PipeStream::write(const char* buffer, std::size_t size)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
        {
            return;
        }
        m_toClient.insert(m_toClient.end(), buffer, buffer + size);
    }
    m_cv.notify_all();
}

void PipeStream::ClientWrite(std::string_view bytes)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
        {
            return;
        }
        m_toServer.insert(m_toServer.end(), bytes.begin(), bytes.end());
    }
    m_cv.notify_all();
}

bool PipeStream::ClientRead(std::string& out, std::size_t size)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this, size] { return m_closed || m_toClient.size() >= size; });
    if (m_toClient.size() < size)
    {
        return false;
    }
    out.assign(m_toClient.begin(), m_toClient.begin() + static_cast<std::ptrdiff_t>(size));
    m_toClient.erase(m_toClient.begin(), m_toClient.begin() + static_cast<std::ptrdiff_t>(size));
    return true;
}

void PipeStream::Close()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
    }
    m_cv.notify_all();
}

} // namespace angel_lsp::mcp
