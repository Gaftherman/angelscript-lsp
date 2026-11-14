#pragma once

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <lsp/io/stream.h>
#include <lsp/json/json.h>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace angel_lsp::test
{

/**
 * @brief Parsed payload from an emitted `textDocument/publishDiagnostics` notification.
 */
struct PublishedDiagnosticFrame
{
    std::string uri;        ///< Document URI reported in notification params.
    int version = -1;       ///< Document version reported in notification params, or -1 if omitted.
    std::string paramsJson; ///< Canonical serialized JSON of the params object for structural comparison.
};

/**
 * @brief Thread-safe blocking in-memory `lsp::io::Stream` for live multi-edit Server tests and benchmarks.
 *
 * Unlike static `ScriptedStream`, `read()` blocks until input bytes arrive or `Close()` is invoked,
 * allowing deterministic `std::latch`/`std::promise`/CV coordination while `Server::Run()` executes.
 */
class LiveLspStream final : public lsp::io::Stream
{
  public:
    /**
     * @brief Appends a Content-Length framed JSON message to the server input stream.
     * @param[in] jsonBody Serialized JSON-RPC request or notification body.
     */
    void PushRawJson(std::string_view jsonBody)
    {
        std::string frame = "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n\r\n";
        frame.append(jsonBody);
        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_input.append(frame);
        }
        m_inputCv.notify_all();
    }

    /**
     * @brief Serializes and enqueues a JSON-RPC value as a framed LSP message.
     * @param[in] message JSON-RPC message object.
     */
    void PushJson(const lsp::json::Value& message)
    {
        PushRawJson(lsp::json::stringify(message));
    }

    /**
     * @brief Marks EOF on the input side so `Server::Run()` exits once queued bytes are consumed.
     */
    void Close()
    {
        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_closed = true;
        }
        m_inputCv.notify_all();
    }

    /**
     * @brief Blocks until `size` bytes are available or the stream is closed at EOF.
     * @param[out] buffer Destination buffer receiving bytes.
     * @param[in] size Number of bytes requested by JSON-RPC connection.
     */
    void read(char* buffer, std::size_t size) override
    {
        size_t copied = 0;
        std::unique_lock<std::mutex> lock(m_inputMutex);
        while (copied < size)
        {
            m_inputCv.wait(lock, [this]() { return m_closed || m_readOffset < m_input.size(); });
            if (m_readOffset >= m_input.size())
            {
                throw lsp::io::Error("end of live test stream");
            }
            const size_t available = m_input.size() - m_readOffset;
            const size_t chunk = std::min(size - copied, available);
            std::memcpy(buffer + copied, m_input.data() + m_readOffset, chunk);
            m_readOffset += chunk;
            copied += chunk;
        }
    }

    /**
     * @brief Appends server output bytes and incrementally parses complete LSP frames.
     * @param[in] buffer Serialized LSP bytes written by the server connection.
     * @param[in] size Byte length of `buffer`.
     */
    void write(const char* buffer, std::size_t size) override
    {
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            m_rawOutput.append(buffer, size);
            ParseCompleteFramesLocked();
        }
        m_outputCv.notify_all();
    }

    /**
     * @brief Blocks until a JSON-RPC response frame with matching `id` is emitted by the server.
     * @param[in] id Request identifier to wait for.
     */
    void WaitForResponseId(int id) const
    {
        std::unique_lock<std::mutex> lock(m_outputMutex);
        m_outputCv.wait(lock, [this, id]() { return m_responseIds.contains(id); });
    }

    /**
     * @brief Blocks until a `textDocument/publishDiagnostics` frame with matching `version` arrives.
     * @param[in] version Document version expected in `params.version`.
     */
    void WaitForPublishedVersion(int version) const
    {
        std::unique_lock<std::mutex> lock(m_outputMutex);
        m_outputCv.wait(lock,
                        [this, version]()
                        {
                            return std::any_of(m_published.begin(), m_published.end(),
                                               [version](const PublishedDiagnosticFrame& frame)
                                               { return frame.version == version; });
                        });
    }

    /**
     * @brief Returns a snapshot copy of all published diagnostics notifications recorded so far.
     * @return Vector of parsed notification frames in arrival order.
     */
    [[nodiscard]] std::vector<PublishedDiagnosticFrame> PublishedFrames() const
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        return m_published;
    }

  private:
    void ParseCompleteFramesLocked()
    {
        constexpr std::string_view kHeaderPrefix = "Content-Length:";
        while (m_parseOffset < m_rawOutput.size())
        {
            const size_t headerStart = m_rawOutput.find(kHeaderPrefix, m_parseOffset);
            if (headerStart == std::string::npos)
            {
                break;
            }
            const size_t lenStart = headerStart + kHeaderPrefix.size();
            const size_t headerEnd = m_rawOutput.find("\r\n\r\n", lenStart);
            if (headerEnd == std::string::npos)
            {
                break;
            }
            size_t digitStart = lenStart;
            while (digitStart < headerEnd && m_rawOutput[digitStart] == ' ')
            {
                ++digitStart;
            }
            size_t contentLength = 0;
            const auto [ptr, ec] =
                std::from_chars(m_rawOutput.data() + digitStart, m_rawOutput.data() + headerEnd, contentLength);
            if (ec != std::errc{})
            {
                m_parseOffset = headerEnd + 4;
                continue;
            }
            const size_t bodyStart = headerEnd + 4;
            if (bodyStart + contentLength > m_rawOutput.size())
            {
                break;
            }
            const std::string_view body(m_rawOutput.data() + bodyStart, contentLength);
            RecordParsedFrameLocked(body);
            m_parseOffset = bodyStart + contentLength;
        }
    }

    void RecordParsedFrameLocked(std::string_view body)
    {
        const lsp::json::Value msg = lsp::json::parse(body);
        if (!msg.isObject())
        {
            return;
        }
        const auto& obj = msg.object();
        if (const auto* idVal = obj.find("id");
            idVal != nullptr && idVal->isInteger() && (obj.find("result") != nullptr || obj.find("error") != nullptr))
        {
            m_responseIds.insert(static_cast<int>(idVal->integer()));
            return;
        }
        const auto* methodVal = obj.find("method");
        if (methodVal == nullptr || !methodVal->isString() || methodVal->string() != "textDocument/publishDiagnostics")
        {
            return;
        }
        const auto* paramsVal = obj.find("params");
        if (paramsVal == nullptr || !paramsVal->isObject())
        {
            return;
        }
        const auto& params = paramsVal->object();
        PublishedDiagnosticFrame frame;
        if (const auto* uriVal = params.find("uri"); uriVal != nullptr && uriVal->isString())
        {
            frame.uri = uriVal->string();
        }
        if (const auto* verVal = params.find("version"); verVal != nullptr && verVal->isInteger())
        {
            frame.version = static_cast<int>(verVal->integer());
        }
        frame.paramsJson = lsp::json::stringify(*paramsVal);
        m_published.push_back(std::move(frame));
    }

    mutable std::mutex m_inputMutex;
    mutable std::condition_variable m_inputCv;
    std::string m_input;
    size_t m_readOffset = 0;
    bool m_closed = false;

    mutable std::mutex m_outputMutex;
    mutable std::condition_variable m_outputCv;
    std::string m_rawOutput;
    size_t m_parseOffset = 0;
    std::unordered_set<int> m_responseIds;
    std::vector<PublishedDiagnosticFrame> m_published;
};

/**
 * @brief Constructs a JSON-RPC `initialize` request targeting `rootUri`.
 * @param[in] id JSON-RPC request identifier.
 * @param[in] rootUri Workspace root URI string.
 * @return Ready-to-push JSON-RPC request object.
 */
inline lsp::json::Value MakeInitializeMessage(int id, std::string_view rootUri)
{
    lsp::json::Object folder;
    folder["uri"] = lsp::json::Value(std::string(rootUri));
    folder["name"] = lsp::json::Value(std::string("fixture"));
    lsp::json::Array folders;
    folders.emplace_back(lsp::json::Value(std::move(folder)));

    lsp::json::Object params;
    params["processId"] = lsp::json::Value{};
    params["rootUri"] = lsp::json::Value(std::string(rootUri));
    params["capabilities"] = lsp::json::Value(lsp::json::Object{});
    params["workspaceFolders"] = lsp::json::Value(std::move(folders));

    lsp::json::Object msg;
    msg["jsonrpc"] = lsp::json::Value(std::string("2.0"));
    msg["id"] = lsp::json::Value(id);
    msg["method"] = lsp::json::Value(std::string("initialize"));
    msg["params"] = lsp::json::Value(std::move(params));
    return lsp::json::Value(std::move(msg));
}

/**
 * @brief Constructs a JSON-RPC notification object with `method` and `params`.
 * @param[in] method LSP notification method name.
 * @param[in] params JSON params object or null value.
 * @return Ready-to-push JSON-RPC notification object.
 */
inline lsp::json::Value MakeNotificationMessage(std::string_view method, lsp::json::Value params)
{
    lsp::json::Object msg;
    msg["jsonrpc"] = lsp::json::Value(std::string("2.0"));
    msg["method"] = lsp::json::Value(std::string(method));
    msg["params"] = std::move(params);
    return lsp::json::Value(std::move(msg));
}

/**
 * @brief Constructs a `textDocument/didOpen` notification.
 * @param[in] uri Target document URI.
 * @param[in] text Full initial document text.
 * @param[in] version Initial document version.
 * @return Ready-to-push JSON-RPC notification object.
 */
inline lsp::json::Value MakeDidOpenMessage(std::string_view uri, std::string_view text, int version = 1)
{
    lsp::json::Object doc;
    doc["uri"] = lsp::json::Value(std::string(uri));
    doc["languageId"] = lsp::json::Value(std::string("angelscript"));
    doc["version"] = lsp::json::Value(version);
    doc["text"] = lsp::json::Value(std::string(text));

    lsp::json::Object params;
    params["textDocument"] = lsp::json::Value(std::move(doc));
    return MakeNotificationMessage("textDocument/didOpen", lsp::json::Value(std::move(params)));
}

/**
 * @brief Constructs a whole-document `textDocument/didChange` notification.
 * @param[in] uri Target document URI.
 * @param[in] version Monotonically increasing document version.
 * @param[in] text Updated full document text.
 * @return Ready-to-push JSON-RPC notification object.
 */
inline lsp::json::Value MakeDidChangeWholeMessage(std::string_view uri, int version, std::string_view text)
{
    lsp::json::Object doc;
    doc["uri"] = lsp::json::Value(std::string(uri));
    doc["version"] = lsp::json::Value(version);

    lsp::json::Object change;
    change["text"] = lsp::json::Value(std::string(text));
    lsp::json::Array changes;
    changes.emplace_back(std::move(change));

    lsp::json::Object params;
    params["textDocument"] = lsp::json::Value(std::move(doc));
    params["contentChanges"] = lsp::json::Value(std::move(changes));
    return MakeNotificationMessage("textDocument/didChange", lsp::json::Value(std::move(params)));
}

/**
 * @brief Constructs a JSON-RPC `shutdown` request.
 * @param[in] id JSON-RPC request identifier.
 * @return Ready-to-push JSON-RPC request object.
 */
inline lsp::json::Value MakeShutdownMessage(int id)
{
    lsp::json::Object msg;
    msg["jsonrpc"] = lsp::json::Value(std::string("2.0"));
    msg["id"] = lsp::json::Value(id);
    msg["method"] = lsp::json::Value(std::string("shutdown"));
    return lsp::json::Value(std::move(msg));
}

} // namespace angel_lsp::test
