/**
 * @file FeatureBenchmarkTest.cpp
 * @brief Skipped-by-default end-to-end latency and RSS memory benchmarks for every AngelLSP LSP feature and combined
 * flow.
 *
 * Drives a real `angel_lsp::Server` instance over `LiveLspStream` (via `CapturingLspStream`) across generated
 * scripts of 1k, 5k, and 20k lines as well as `tests/fixtures/sdk-addons.as.predefined`, plus four combined
 * editor flows (inlayHint -> hover on predefined/included targets both unopened and already open, burst edits ->
 * diagnostics/inlayHint/semanticTokens, character-by-character typing completion, and hover/signatureHelp during
 * in-flight background analysis).
 */

#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include "helpers/LiveLspStream.h"
#include "helpers/LspSemanticHarnessFixture.h"
#include "helpers/ProcessMemory.h"
#include "helpers/TestUtils.h"
#include "lsp/Server.h"
#include "lsp/state/SyntaxRevisionCache.h"
#include "utils/IncludeResolver.h"
#include "utils/Timer.h"
#include "utils/TransparentHash.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <lsp/io/stream.h>
#include <lsp/json/json.h>
#include <lsp/types.h>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace feature_bench
{

/**
 * @brief Fixed seed for deterministic benchmark script generation.
 */
inline constexpr uint64_t kBenchmarkSeed = 20261115ULL;

/**
 * @brief Summary statistics (in milliseconds) computed from a sample vector.
 */
struct LatencySummary
{
    double p50Ms = 0.0; ///< Median latency (50th percentile, nearest-rank).
    double p95Ms = 0.0; ///< Tail latency (95th percentile, nearest-rank).
    double maxMs = 0.0; ///< Maximum observed latency.
};

/**
 * @brief Parsed `textDocument/publishDiagnostics` frame captured from server output.
 */
struct CapturedDiagnosticFrame
{
    std::string uriKey;           ///< Canonicalized document key from `Server::DocumentKey`.
    int version = -1;             ///< Published document version, or -1 if omitted.
    lsp::json::Array diagnostics; ///< Published diagnostic JSON items.
};

/**
 * @brief Stream adapter wrapping `LiveLspStream` to capture JSON-RPC responses and notifications deterministically.
 */
class CapturingLspStream final : public lsp::io::Stream
{
  public:
    /**
     * @brief Enqueues a JSON-RPC message on the underlying `LiveLspStream`.
     * @param[in] message JSON-RPC request or notification object.
     */
    void PushJson(const lsp::json::Value& message)
    {
        m_inner.PushJson(message);
    }

    /**
     * @brief Closes the underlying input stream at EOF so `Server::Run()` terminates cleanly.
     */
    void Close()
    {
        m_inner.Close();
    }

    /**
     * @brief Reads `size` bytes from the underlying `LiveLspStream`.
     * @param[out] buffer Destination buffer.
     * @param[in] size Number of bytes requested.
     */
    void read(char* buffer, std::size_t size) override
    {
        m_inner.read(buffer, size);
    }

    /**
     * @brief Captures server output frames for synchronous inspection and forwards to `LiveLspStream`.
     * @param[in] buffer Serialized LSP bytes emitted by the server.
     * @param[in] size Byte length of `buffer`.
     */
    void write(const char* buffer, std::size_t size) override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_rawOutput.append(buffer, size);
            ParseCompleteFramesLocked();
        }
        m_cv.notify_all();
        m_inner.write(buffer, size);
    }

    /**
     * @brief Blocks until the JSON-RPC response for `id` arrives and returns it.
     * @param[in] id JSON-RPC request identifier.
     * @return Parsed JSON-RPC response object.
     */
    [[nodiscard]] lsp::json::Value WaitForResponse(int id)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this, id]() { return m_responses.contains(id); });
        return m_responses.at(id);
    }

    /**
     * @brief Blocks until `publishDiagnostics` arrives for `(uri, version)` and returns its diagnostics.
     * @param[in] uri Target document URI.
     * @param[in] version Expected document version.
     * @return Copy of the published diagnostic array for that frame.
     */
    [[nodiscard]] lsp::json::Array WaitForPublished(std::string_view uri, int version)
    {
        const std::string key = angel_lsp::Server::DocumentKey(uri);
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto match = [&key, version](const CapturedDiagnosticFrame& frame)
        { return frame.version == version && frame.uriKey == key; };
        m_cv.wait(lock, [this, &match]() { return std::any_of(m_published.begin(), m_published.end(), match); });
        for (auto it = m_published.rbegin(); it != m_published.rend(); ++it)
        {
            if (match(*it))
            {
                return it->diagnostics;
            }
        }
        return {};
    }

    /**
     * @brief Blocks until at least `targetCount` `$/progress` end notifications have been received.
     * @param[in] targetCount Minimum number of end progress notifications to wait for.
     */
    void WaitForProgressEnd(size_t targetCount = 1)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this, targetCount]() { return m_progressEndCount >= targetCount; });
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
            RecordFrameLocked(body);
            m_parseOffset = bodyStart + contentLength;
        }
    }

    void RecordFrameLocked(std::string_view body)
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
            m_responses.insert_or_assign(static_cast<int>(idVal->integer()), msg);
            return;
        }
        const auto* methodVal = obj.find("method");
        if (methodVal == nullptr || !methodVal->isString())
        {
            return;
        }
        const std::string_view method = methodVal->string();
        const auto* paramsVal = obj.find("params");
        if (paramsVal == nullptr || !paramsVal->isObject())
        {
            return;
        }
        const auto& params = paramsVal->object();
        if (method == "textDocument/publishDiagnostics")
        {
            CapturedDiagnosticFrame frame;
            if (const auto* uriVal = params.find("uri"); uriVal != nullptr && uriVal->isString())
            {
                frame.uriKey = angel_lsp::Server::DocumentKey(uriVal->string());
            }
            if (const auto* verVal = params.find("version"); verVal != nullptr && verVal->isInteger())
            {
                frame.version = static_cast<int>(verVal->integer());
            }
            if (const auto* diagVal = params.find("diagnostics"); diagVal != nullptr && diagVal->isArray())
            {
                frame.diagnostics = diagVal->array();
            }
            m_published.push_back(std::move(frame));
        }
        else if (method == "$/progress")
        {
            const auto* val = params.find("value");
            if (val != nullptr && val->isObject())
            {
                if (const auto* kind = val->object().find("kind");
                    kind != nullptr && kind->isString() && kind->string() == "end")
                {
                    ++m_progressEndCount;
                }
            }
        }
    }

    angel_lsp::test::LiveLspStream m_inner;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    std::string m_rawOutput;
    size_t m_parseOffset = 0;
    std::unordered_map<int, lsp::json::Value> m_responses;
    std::vector<CapturedDiagnosticFrame> m_published;
    size_t m_progressEndCount = 0;
};

/**
 * @brief Live `angel_lsp::Server` session running on a dedicated thread with synchronous JSON-RPC helpers.
 */
class BenchmarkSession
{
  public:
    /**
     * @brief Constructs and initializes a `Server` targeting `rootUri`, waiting for workspace scan completion.
     * @param[in] config Server configuration (copied into `Server`).
     * @param[in] rootUri Workspace root `file://` URI.
     */
    BenchmarkSession(const angel_lsp::config::ServerConfig& config, std::string_view rootUri)
        : m_server(config, m_stream), m_thread([this]() { m_server.Run(); })
    {
        lsp::json::Value initMsg = angel_lsp::test::MakeInitializeMessage(m_nextId, rootUri);
        const int initId = m_nextId++;
        lsp::json::Object windowCaps;
        windowCaps["workDoneProgress"] = lsp::json::Value(true);
        initMsg.object()["params"].object()["capabilities"].object()["window"] =
            lsp::json::Value(std::move(windowCaps));

        m_stream.PushJson(initMsg);
        (void)m_stream.WaitForResponse(initId);
        m_stream.PushJson(
            angel_lsp::test::MakeNotificationMessage("initialized", lsp::json::Value(lsp::json::Object{})));
        m_stream.WaitForProgressEnd(1);
    }

    /**
     * @brief Shuts down and joins the server thread if still active.
     */
    ~BenchmarkSession()
    {
        Stop();
    }

    BenchmarkSession(const BenchmarkSession&) = delete;
    BenchmarkSession& operator=(const BenchmarkSession&) = delete;

    /**
     * @brief Returns a mutable reference to the underlying `Server` instance (e.g. for hooks).
     * @return Reference to the active `Server`.
     */
    [[nodiscard]] angel_lsp::Server& ServerRef() noexcept
    {
        return m_server;
    }

    /**
     * @brief Allocates the next monotonically increasing document version for `uri`.
     * @param[in] uri Document URI.
     * @return Next integer version (>= 1).
     */
    [[nodiscard]] int NextVersion(std::string_view uri)
    {
        const std::string key = angel_lsp::Server::DocumentKey(uri);
        auto it = m_docVersions.find(key);
        if (it == m_docVersions.end())
        {
            it = m_docVersions.emplace(key, 0).first;
        }
        return ++(it->second);
    }

    /**
     * @brief Sends `textDocument/didOpen` and returns the allocated document version without waiting.
     * @param[in] uri Document URI.
     * @param[in] text Document text.
     * @return Version assigned to this `didOpen`.
     */
    int SendDidOpen(std::string_view uri, std::string_view text)
    {
        const int version = NextVersion(uri);
        m_stream.PushJson(angel_lsp::test::MakeDidOpenMessage(uri, text, version));
        return version;
    }

    /**
     * @brief Sends `textDocument/didOpen` and waits until `publishDiagnostics` is emitted for that version.
     * @param[in] uri Document URI.
     * @param[in] text Document text.
     * @return Published diagnostic JSON array.
     */
    lsp::json::Array OpenAndWait(std::string_view uri, std::string_view text)
    {
        const int version = SendDidOpen(uri, text);
        return m_stream.WaitForPublished(uri, version);
    }

    /**
     * @brief Sends `textDocument/didClose` for `uri`.
     * @param[in] uri Document URI to close.
     */
    void SendDidClose(std::string_view uri)
    {
        lsp::json::Object doc;
        doc["uri"] = lsp::json::Value(std::string(uri));
        lsp::json::Object params;
        params["textDocument"] = lsp::json::Value(std::move(doc));
        m_stream.PushJson(
            angel_lsp::test::MakeNotificationMessage("textDocument/didClose", lsp::json::Value(std::move(params))));
    }

    /**
     * @brief Sends whole-document `textDocument/didChange` and returns its allocated version.
     * @param[in] uri Document URI.
     * @param[in] text Replacement document text.
     * @return Allocated document version.
     */
    int SendDidChangeWhole(std::string_view uri, std::string_view text)
    {
        const int version = NextVersion(uri);
        m_stream.PushJson(angel_lsp::test::MakeDidChangeWholeMessage(uri, version, text));
        return version;
    }

    /**
     * @brief Sends an incremental `textDocument/didChange` replacing `range` with `text`.
     * @param[in] uri Document URI.
     * @param[in] range LSP range to replace.
     * @param[in] text Replacement text.
     * @return Allocated document version.
     */
    int SendDidChangeRange(std::string_view uri, const lsp::Range& range, std::string_view text)
    {
        const int version = NextVersion(uri);
        lsp::json::Object doc;
        doc["uri"] = lsp::json::Value(std::string(uri));
        doc["version"] = lsp::json::Value(version);

        lsp::json::Object startObj;
        startObj["line"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.start.line));
        startObj["character"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.start.character));
        lsp::json::Object endObj;
        endObj["line"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.end.line));
        endObj["character"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.end.character));
        lsp::json::Object rangeObj;
        rangeObj["start"] = lsp::json::Value(std::move(startObj));
        rangeObj["end"] = lsp::json::Value(std::move(endObj));

        lsp::json::Object change;
        change["range"] = lsp::json::Value(std::move(rangeObj));
        change["text"] = lsp::json::Value(std::string(text));
        lsp::json::Array changes;
        changes.emplace_back(std::move(change));

        lsp::json::Object params;
        params["textDocument"] = lsp::json::Value(std::move(doc));
        params["contentChanges"] = lsp::json::Value(std::move(changes));
        m_stream.PushJson(
            angel_lsp::test::MakeNotificationMessage("textDocument/didChange", lsp::json::Value(std::move(params))));
        return version;
    }

    /**
     * @brief Blocks until `publishDiagnostics` arrives for `(uri, version)`.
     * @param[in] uri Document URI.
     * @param[in] version Expected published version.
     * @return Published diagnostics JSON array.
     */
    lsp::json::Array WaitForPublished(std::string_view uri, int version)
    {
        return m_stream.WaitForPublished(uri, version);
    }

    /**
     * @brief Enqueues a JSON-RPC request and returns its allocated request identifier without waiting.
     * @param[in] method LSP method name.
     * @param[in] params JSON-RPC params value.
     * @return Allocated JSON-RPC request id.
     */
    int SendRequest(std::string_view method, lsp::json::Value params)
    {
        const int id = m_nextId++;
        lsp::json::Object msg;
        msg["jsonrpc"] = lsp::json::Value(std::string("2.0"));
        msg["id"] = lsp::json::Value(id);
        msg["method"] = lsp::json::Value(std::string(method));
        msg["params"] = std::move(params);
        m_stream.PushJson(lsp::json::Value(std::move(msg)));
        return id;
    }

    /**
     * @brief Blocks for the response to a previously dispatched `SendRequest`.
     * @param[in] id Request identifier returned by `SendRequest`.
     * @return JSON-RPC response object.
     */
    [[nodiscard]] lsp::json::Value WaitForResponse(int id)
    {
        return m_stream.WaitForResponse(id);
    }

    /**
     * @brief Sends a JSON-RPC request synchronously and waits for its response.
     * @param[in] method LSP method name.
     * @param[in] params JSON-RPC params value.
     * @return JSON-RPC response object.
     */
    [[nodiscard]] lsp::json::Value Request(std::string_view method, lsp::json::Value params)
    {
        const int id = SendRequest(method, std::move(params));
        return m_stream.WaitForResponse(id);
    }

    /**
     * @brief Cleanly shuts down the LSP session and joins the server thread.
     */
    void Stop()
    {
        if (m_stopped)
        {
            return;
        }
        m_stopped = true;
        const int shutdownId = m_nextId++;
        m_stream.PushJson(angel_lsp::test::MakeShutdownMessage(shutdownId));
        (void)m_stream.WaitForResponse(shutdownId);
        m_stream.PushJson(angel_lsp::test::MakeNotificationMessage("exit", lsp::json::Value{}));
        m_stream.Close();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

  private:
    CapturingLspStream m_stream;
    angel_lsp::Server m_server;
    std::thread m_thread;
    int m_nextId = 1;
    bool m_stopped = false;
    ankerl::unordered_dense::map<std::string, int, angel_lsp::TransparentStringHash, std::equal_to<>> m_docVersions;
};

/**
 * @brief Computes nearest-rank p50, p95, and max from a sample vector.
 * @param[in] samples Latency observations in milliseconds.
 * @return Aggregated `LatencySummary`.
 */
inline LatencySummary ComputeLatencySummary(const std::vector<double>& samples)
{
    if (samples.empty())
    {
        return {};
    }
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    const size_t n = sorted.size();
    auto nearestRank = [&sorted, n](size_t percentile) -> double
    {
        size_t rank = (n * percentile + 99) / 100;
        if (rank == 0)
        {
            rank = 1;
        }
        return sorted[rank - 1];
    };

    return LatencySummary{
        .p50Ms = nearestRank(50),
        .p95Ms = nearestRank(95),
        .maxMs = sorted.back(),
    };
}

/**
 * @brief Computes signed resident memory delta in MiB using `CurrentResidentBytes()`.
 * @param[in] beforeBytes Resident bytes before the measured work.
 * @param[in] afterBytes Resident bytes after the measured work.
 * @return Delta in MiB (`(after - before) / 1048576.0`).
 */
inline double ComputeRssDeltaMib(size_t beforeBytes, size_t afterBytes) noexcept
{
    constexpr double kBytesPerMib = 1024.0 * 1024.0;
    return (static_cast<double>(afterBytes) - static_cast<double>(beforeBytes)) / kBytesPerMib;
}

/**
 * @brief Emits the markdown benchmark table header via doctest `MESSAGE`.
 */
inline void PrintTableHeader()
{
    MESSAGE("| feature | size | n | p50 ms | p95 ms | max ms | RSS delta MiB |");
    MESSAGE("| --- | --- | --- | --- | --- | --- | --- |");
}

/**
 * @brief Formats and prints a single markdown benchmark result row via doctest `MESSAGE`.
 * @param[in] feature Descriptive feature or flow label.
 * @param[in] sizeLabel Workload size label (e.g. `"5000 lines"`, `"sdk-addons.as.predefined"`).
 * @param[in] samples Latency samples in milliseconds.
 * @param[in] rssDeltaMib Resident memory delta in MiB.
 */
inline void PrintTableRow(std::string_view feature, std::string_view sizeLabel, const std::vector<double>& samples,
                          double rssDeltaMib)
{
    const LatencySummary summary = ComputeLatencySummary(samples);
    std::ostringstream row;
    row << std::fixed << std::setprecision(3) << "| " << feature << " | " << sizeLabel << " | " << samples.size()
        << " | " << summary.p50Ms << " | " << summary.p95Ms << " | " << summary.maxMs << " | " << std::setprecision(2)
        << rssDeltaMib << " |";
    MESSAGE(row.str());
}

/**
 * @brief Computes 0-based `(line, character)` coordinates for a byte offset in `text`.
 * @param[in] text UTF-8 source buffer.
 * @param[in] offset Byte offset inside `text`.
 * @return 0-based `lsp::Position`.
 */
inline lsp::Position PositionAtOffset(std::string_view text, size_t offset) noexcept
{
    const size_t clamped = std::min(offset, text.size());
    const uint32_t line =
        static_cast<uint32_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(clamped), '\n'));
    const size_t lastNl = text.rfind('\n', clamped == 0 ? 0 : clamped - 1);
    const size_t lineStart = (lastNl == std::string_view::npos) ? 0 : lastNl + 1;
    const uint32_t col = static_cast<uint32_t>(clamped - lineStart);
    return lsp::Position{line, col};
}

/**
 * @brief Finds the first occurrence of `needle` at or after `fromOffset` and returns its 0-based position.
 * @param[in] text Source text to search.
 * @param[in] needle Substring to locate.
 * @param[in] fromOffset Starting byte offset.
 * @return `lsp::Position` of the start of `needle`, or `std::nullopt` if not found.
 */
inline std::optional<lsp::Position> FindTokenPos(std::string_view text, std::string_view needle, size_t fromOffset = 0)
{
    const size_t pos = text.find(needle, fromOffset);
    if (pos == std::string_view::npos)
    {
        return std::nullopt;
    }
    return PositionAtOffset(text, pos);
}

/**
 * @brief Writes a UTF-8 file relative to `rootDir`, creating parent directories as needed.
 * @param[in] rootDir Sandbox root directory.
 * @param[in] relPath Relative path inside `rootDir`.
 * @param[in] content File contents to write.
 * @return Normalized filesystem path of the created file.
 */
inline std::string WriteSandboxFile(const std::filesystem::path& rootDir, std::string_view relPath,
                                    std::string_view content)
{
    const std::filesystem::path fullPath = rootDir / relPath;
    if (fullPath.has_parent_path())
    {
        std::filesystem::create_directories(fullPath.parent_path());
    }
    std::ofstream out(fullPath, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return angel_lsp::utils::IncludeResolver::NormalizePath(fullPath);
}

/**
 * @brief Converts a `lsp::Position` to its JSON representation.
 * @param[in] pos 0-based LSP position.
 * @return JSON object `{"line": ..., "character": ...}`.
 */
inline lsp::json::Value ToJsonPosition(const lsp::Position& pos)
{
    lsp::json::Object obj;
    obj["line"] = lsp::json::Value(static_cast<lsp::json::Integer>(pos.line));
    obj["character"] = lsp::json::Value(static_cast<lsp::json::Integer>(pos.character));
    return lsp::json::Value(std::move(obj));
}

/**
 * @brief Converts a `lsp::Range` to its JSON representation.
 * @param[in] range 0-based LSP range.
 * @return JSON object `{"start": ..., "end": ...}`.
 */
inline lsp::json::Value ToJsonRange(const lsp::Range& range)
{
    lsp::json::Object obj;
    obj["start"] = ToJsonPosition(range.start);
    obj["end"] = ToJsonPosition(range.end);
    return lsp::json::Value(std::move(obj));
}

/**
 * @brief Builds `{"textDocument":{"uri":...}}` params for document-level LSP requests.
 * @param[in] uri Target document URI.
 * @return JSON-RPC params object.
 */
inline lsp::json::Value MakeTextDocParams(std::string_view uri)
{
    lsp::json::Object doc;
    doc["uri"] = lsp::json::Value(std::string(uri));
    lsp::json::Object params;
    params["textDocument"] = lsp::json::Value(std::move(doc));
    return lsp::json::Value(std::move(params));
}

/**
 * @brief Builds `{"textDocument":{"uri":...},"position":...}` params for position-based LSP requests.
 * @param[in] uri Target document URI.
 * @param[in] pos 0-based cursor position.
 * @return JSON-RPC params object.
 */
inline lsp::json::Value MakePositionParams(std::string_view uri, const lsp::Position& pos)
{
    lsp::json::Value params = MakeTextDocParams(uri);
    params.object()["position"] = ToJsonPosition(pos);
    return params;
}

/**
 * @brief Builds `{"textDocument":{"uri":...},"range":...}` params for range-based LSP requests.
 * @param[in] uri Target document URI.
 * @param[in] range 0-based LSP range.
 * @return JSON-RPC params object.
 */
inline lsp::json::Value MakeRangeParams(std::string_view uri, const lsp::Range& range)
{
    lsp::json::Value params = MakeTextDocParams(uri);
    params.object()["range"] = ToJsonRange(range);
    return params;
}

/**
 * @brief Builds default LSP `FormattingOptions` JSON (`tabSize: 4`, `insertSpaces: true`).
 * @return JSON object matching `lsp::FormattingOptions`.
 */
inline lsp::json::Value MakeFormattingOptions()
{
    lsp::json::Object opts;
    opts["tabSize"] = lsp::json::Value(4);
    opts["insertSpaces"] = lsp::json::Value(true);
    return lsp::json::Value(std::move(opts));
}

/**
 * @brief Holds a generated benchmark script and every probe coordinate required to exercise all LSP features.
 */
struct GeneratedFeatureScript
{
    std::string source;             ///< Full generated AngelScript source text.
    size_t lineCount = 0;           ///< Total line count of `source`.
    lsp::Position localVarPos{};    ///< Local variable inside `ProbeDerived::Execute`.
    lsp::Position memberPos{};      ///< Member access `m_probeField` on `probeObj`.
    lsp::Position predefPos{};      ///< Predefined function call `formatInt`.
    lsp::Position includedPos{};    ///< Included function call `IncludedApi::ComputeShared`.
    lsp::Position typeDefPos{};     ///< Variable `probeObj` for `typeDefinition`.
    lsp::Position implPos{};        ///< Interface method `Execute` for `implementation`.
    lsp::Position sigHelpPos{};     ///< Inside `CalleeTarget(total, |1.0f)` for `signatureHelp`.
    lsp::Position memberDotPos{};   ///< Immediately after `dotObj.` for member completion.
    lsp::Position scopeColonPos{};  ///< Immediately after `IncludedApi::` for scope completion.
    lsp::Position callTargetPos{};  ///< `CalleeTarget` declaration for incoming call hierarchy.
    lsp::Position callWrapperPos{}; ///< `CallerWrapper` declaration for outgoing call hierarchy.
    lsp::Position typeBasePos{};    ///< `ProbeBase` declaration for type hierarchy subtypes.
    lsp::Position typeDerivedPos{}; ///< `ProbeDerived` declaration for type hierarchy supertypes.
    lsp::Position onTypePos{};      ///< Immediately after `;` in `total += sigCall;`.
    lsp::Position typingDotPos{};   ///< Position right after `typeObj.` for char-by-char completion.
    lsp::Range codeActionRange{};   ///< Range covering `inputVal + 1` for extract-variable / diagnostics.
    lsp::Range formatRange{};       ///< 20-line range inside generated body for range formatting.
    uint32_t tailEditLine = 0;      ///< Trailing comment line safe to mutate without shifting probes.
};

/**
 * @brief Returns the shared included AngelScript library source used across multi-file and inlay-hover benchmarks.
 * @return AngelScript source for `included_lib.as`.
 */
inline std::string BuildIncludedLibrarySource()
{
    return "namespace IncludedApi\n"
           "{\n"
           "    class SharedBase\n"
           "    {\n"
           "        int baseValue;\n"
           "        void BaseWork() {}\n"
           "    }\n\n"
           "    /// Computes a scaled offset from two parameters.\n"
           "    int ComputeShared(int sharedAlpha, float sharedBeta = 1.5f)\n"
           "    {\n"
           "        return sharedAlpha + int(sharedBeta);\n"
           "    }\n"
           "}\n";
}

/**
 * @brief Generates a valid AngelScript benchmark file of at least `targetLines` lines with known feature probes.
 * @param[in] targetLines Minimum line count to generate (e.g. 1000, 5000, 20000).
 * @param[in,out] rng Seeded Mersenne Twister generator for deterministic filler coefficients.
 * @return Populated `GeneratedFeatureScript`.
 */
inline GeneratedFeatureScript GenerateFeatureBenchmarkScript(size_t targetLines, std::mt19937_64& rng)
{
    GeneratedFeatureScript out;
    std::string& code = out.source;
    code.reserve(targetLines * 52);

    code += "#include \"included_lib.as\"\n\n";
    code += "interface IBenchmarkTask\n{\n    int Execute(int inputVal);\n}\n\n";
    code += "class ProbeBase : IncludedApi::SharedBase\n{\n"
            "    int m_probeField;\n"
            "    float m_extraField;\n"
            "    void ResetField() { m_probeField = 0; }\n"
            "}\n\n";
    code += "class ProbeDerived : ProbeBase, IBenchmarkTask\n{\n"
            "    int Execute(int inputVal)\n"
            "    {\n"
            "        int localAccum = inputVal + 1;\n"
            "        localAccum += m_probeField;\n"
            "        return localAccum;\n"
            "    }\n"
            "}\n\n";
    code += "int CalleeTarget(int targetAlpha, float targetBeta)\n{\n"
            "    return targetAlpha + int(targetBeta);\n"
            "}\n\n";
    code += "int CallerWrapper(int wrapSeed)\n{\n"
            "    int fromLocal = CalleeTarget(wrapSeed, 2.0f);\n"
            "    int fromInclude = IncludedApi::ComputeShared(fromLocal, 3.0f);\n"
            "    string fromPredef = formatInt(fromInclude, \"0\", 4);\n"
            "    return int(fromPredef.length());\n"
            "}\n\n";

    code += "namespace BenchmarkModule\n{\n";
    size_t currentLine = static_cast<size_t>(std::count(code.begin(), code.end(), '\n'));
    size_t classIdx = 0;
    std::uniform_int_distribution<int> coeffDist(2, 9);
    const size_t midpointLine = targetLines / 2;
    bool recordedFormatRange = false;

    while (currentLine + 35 < targetLines)
    {
        const std::string clsName = angel_lsp::test::GenerateIdentifier(rng, "Cls") + std::to_string(classIdx++);
        code += "    class " + clsName + "\n    {\n";
        code += "        int m_id;\n";
        code += "        float m_score;\n";
        code += "        bool m_active;\n\n";
        currentLine += 6;

        for (int m = 0; m < 10 && currentLine + 35 < targetLines; ++m)
        {
            const std::string fnName = angel_lsp::test::GenerateIdentifier(rng, "Compute") + std::to_string(m);
            const int coeff = coeffDist(rng);
            if (!recordedFormatRange && currentLine >= midpointLine)
            {
                out.formatRange.start = lsp::Position{static_cast<uint32_t>(currentLine), 0};
                out.formatRange.end = lsp::Position{static_cast<uint32_t>(currentLine + 14), 0};
                recordedFormatRange = true;
            }
            code += "        int " + fnName + "(int a, float b)\n        {\n";
            code += "            int sum = a + " + std::to_string(coeff) + ";\n";
            code += "            float mult = b * 2.0f;\n";
            code += "            for (int i = 0; i < 5; ++i)\n            {\n";
            code += "                sum += i * 3;\n";
            code += "                if (sum > 100)\n                {\n";
            code += "                    sum -= 50;\n";
            code += "                }\n";
            code += "            }\n";
            code += "            return sum + int(mult);\n";
            code += "        }\n\n";
            currentLine += 14;
        }

        code += "    }\n\n";
        currentLine += 2;
    }

    code += "}\n\n";

    const size_t tailOffset = code.size();
    code += "void BenchmarkEntry()\n{\n"
            "    int unusedProbe = 99;\n"
            "    ProbeDerived probeObj;\n"
            "    int memberVal = probeObj.m_probeField;\n"
            "    int total = CallerWrapper(memberVal);\n"
            "    int sigCall = CalleeTarget(total, 1.0f);\n"
            "    total += sigCall;\n"
            "}\n\n";

    code += "void CompletionMemberProbe()\n{\n"
            "    ProbeDerived dotObj;\n"
            "    dotObj.\n"
            "}\n\n";

    code += "void CompletionScopeProbe()\n{\n"
            "    IncludedApi::\n"
            "}\n\n";

    code += "void TypingProbe()\n{\n"
            "    ProbeDerived typeObj;\n"
            "    typeObj.\n"
            "}\n\n";

    const size_t editCommentOffset = code.size();
    code += "// trailing edit anchor\n";

    out.lineCount = static_cast<size_t>(std::count(code.begin(), code.end(), '\n') + 1);
    if (!recordedFormatRange)
    {
        const uint32_t mid = static_cast<uint32_t>(out.lineCount / 2);
        out.formatRange = lsp::Range{lsp::Position{mid, 0}, lsp::Position{mid + 10, 0}};
    }

    const auto requirePos = [&](std::string_view needle, size_t fromOffset = 0) -> lsp::Position
    {
        const auto pos = FindTokenPos(code, needle, fromOffset);
        REQUIRE(pos.has_value());
        return *pos;
    };

    out.implPos = requirePos("Execute(int inputVal);");
    out.localVarPos = requirePos("localAccum += m_probeField;");
    out.callTargetPos = requirePos("CalleeTarget(int targetAlpha");
    out.callWrapperPos = requirePos("CallerWrapper(int wrapSeed)");
    out.typeBasePos = requirePos("ProbeBase : IncludedApi");
    out.typeDerivedPos = requirePos("ProbeDerived : ProbeBase");
    out.includedPos = requirePos("ComputeShared(fromLocal");
    out.predefPos = requirePos("formatInt(fromInclude");

    const lsp::Position extractStart = requirePos("inputVal + 1");
    out.codeActionRange = lsp::Range{extractStart, lsp::Position{extractStart.line, extractStart.character + 12}};

    out.memberPos = requirePos("m_probeField;", tailOffset);
    out.typeDefPos = requirePos("probeObj.m_probeField;", tailOffset);

    const lsp::Position sigAnchor = requirePos("CalleeTarget(total, ", tailOffset);
    out.sigHelpPos = lsp::Position{sigAnchor.line, sigAnchor.character + 20};

    const lsp::Position semiAnchor = requirePos("total += sigCall;", tailOffset);
    out.onTypePos = lsp::Position{semiAnchor.line, semiAnchor.character + 17};

    const lsp::Position dotAnchor = requirePos("dotObj.", tailOffset);
    out.memberDotPos = lsp::Position{dotAnchor.line, dotAnchor.character + 7};

    const lsp::Position scopeAnchor = requirePos("IncludedApi::", tailOffset);
    out.scopeColonPos = lsp::Position{scopeAnchor.line, scopeAnchor.character + 13};

    const lsp::Position typeAnchor = requirePos("typeObj.", tailOffset);
    out.typingDotPos = lsp::Position{typeAnchor.line, typeAnchor.character + 8};

    out.tailEditLine = PositionAtOffset(code, editCommentOffset).line;
    return out;
}

/**
 * @brief Extracts the `"result"` member of a JSON-RPC response, failing the check if `"error"` is present.
 * @param[in] response Full JSON-RPC response object.
 * @return Pointer to `"result"` value, or nullptr if missing/error.
 */
inline const lsp::json::Value* ExtractResult(const lsp::json::Value& response)
{
    if (!response.isObject())
    {
        return nullptr;
    }
    const auto& obj = response.object();
    if (const auto* err = obj.find("error"); err != nullptr)
    {
        FAIL_CHECK("Unexpected LSP error: " << lsp::json::stringify(*err));
        return nullptr;
    }
    return obj.find("result");
}

/**
 * @brief Measures `iterations` invocations of a single JSON-RPC request and records a markdown table row.
 * @param[in,out] session Active benchmark server session.
 * @param[in] label Feature name for the markdown table.
 * @param[in] sizeLabel Size label for the markdown table.
 * @param[in] spec Method name, params payload, and iteration count.
 * @return Last JSON-RPC response received (for chaining resolve/hierarchy requests).
 */
struct RequestBenchSpec
{
    std::string_view method;
    lsp::json::Value params;
    size_t iterations = 10;
};

/**
 * @brief Executes `spec.iterations` requests, computes latency summary + RSS delta, and prints a markdown row.
 * @param[in,out] session Active benchmark session.
 * @param[in] label Row feature label.
 * @param[in] sizeLabel Row workload size label.
 * @param[in] spec Method, parameters, and iteration count.
 * @return Final JSON-RPC response from the measured loop.
 */
inline lsp::json::Value BenchmarkRequest(BenchmarkSession& session, std::string_view label, std::string_view sizeLabel,
                                         const RequestBenchSpec& spec)
{
    std::vector<double> samples;
    samples.reserve(spec.iterations);
    lsp::json::Value lastResp;
    const size_t rssBefore = angel_lsp::test::CurrentResidentBytes();

    for (size_t i = 0; i < spec.iterations; ++i)
    {
        angel_lsp::utils::HighResTimer timer;
        lsp::json::Value resp = session.Request(spec.method, spec.params);
        samples.push_back(timer.ElapsedMs());
        if (i + 1 == spec.iterations)
        {
            lastResp = std::move(resp);
        }
    }

    const size_t rssAfter = angel_lsp::test::CurrentResidentBytes();
    PrintTableRow(label, sizeLabel, samples, ComputeRssDeltaMib(rssBefore, rssAfter));
    return lastResp;
}

/**
 * @brief Selects the best completion item from a `textDocument/completion` response for `completionItem/resolve`.
 * @param[in] completionResp JSON-RPC response from `textDocument/completion`.
 * @return Selected item JSON object, or `std::nullopt` when no items were returned.
 */
inline std::optional<lsp::json::Value> PickCompletionItemForResolve(const lsp::json::Value& completionResp)
{
    const auto* res = ExtractResult(completionResp);
    if (res == nullptr)
    {
        return std::nullopt;
    }
    const lsp::json::Array* items = nullptr;
    if (res->isArray())
    {
        items = &res->array();
    }
    else if (res->isObject())
    {
        if (const auto* arr = res->object().find("items"); arr != nullptr && arr->isArray())
        {
            items = &arr->array();
        }
    }
    if (items == nullptr || items->empty())
    {
        return std::nullopt;
    }
    for (const auto& item : *items)
    {
        if (item.isObject() && item.object().find("data") != nullptr && item.object().find("documentation") == nullptr)
        {
            return item;
        }
    }
    return items->front();
}

/**
 * @brief Returns the first element of an array `"result"` in a JSON-RPC response, or `std::nullopt`.
 * @param[in] resp JSON-RPC response object.
 * @return First array element if present and non-empty.
 */
inline std::optional<lsp::json::Value> PickFirstArrayElement(const lsp::json::Value& resp)
{
    const auto* res = ExtractResult(resp);
    if (res == nullptr || !res->isArray() || res->array().empty())
    {
        return std::nullopt;
    }
    return res->array().front();
}

/**
 * @brief Extracts `resultId` string from a `textDocument/semanticTokens/full` or `/delta` response.
 * @param[in] resp JSON-RPC response object.
 * @return `resultId` string if present.
 */
inline std::optional<std::string> ExtractSemanticResultId(const lsp::json::Value& resp)
{
    const auto* res = ExtractResult(resp);
    if (res == nullptr || !res->isObject())
    {
        return std::nullopt;
    }
    const auto* idVal = res->object().find("resultId");
    if (idVal == nullptr || !idVal->isString())
    {
        return std::nullopt;
    }
    return idVal->string();
}

/**
 * @brief Runs every individual LSP feature benchmark on a generated script of `targetLines` lines.
 * @param[in] targetLines Minimum script line count (1000, 5000, or 20000).
 * @param[in] sizeLabel Human-readable size label for the markdown table.
 */
inline void RunGeneratedScriptFeatureBenchmarks(size_t targetLines, std::string_view sizeLabel)
{
    angel_lsp::test::LspSemanticHarnessFixture fixture;
    const std::string predefText = angel_lsp::test::LoadFixtureText("sdk-addons.as.predefined");
    REQUIRE(!predefText.empty());
    WriteSandboxFile(fixture.GetSandboxDir(), "sdk-addons.as.predefined", predefText);
    WriteSandboxFile(fixture.GetSandboxDir(), "included_lib.as", BuildIncludedLibrarySource());

    std::mt19937_64 rng(kBenchmarkSeed + targetLines);
    const GeneratedFeatureScript script = GenerateFeatureBenchmarkScript(targetLines, rng);
    REQUIRE(script.lineCount >= targetLines);

    WriteSandboxFile(fixture.GetSandboxDir(), "main.as", script.source);
    const std::string mainUri = fixture.SandboxUri("main.as");
    const std::string rootUri = angel_lsp::utils::PathToUri(fixture.GetSandboxDir().string());

    angel_lsp::config::ServerConfig serverConfig;
    serverConfig.features.enableOnTypeFormatting = true;
    BenchmarkSession session(serverConfig, rootUri);

    const size_t fastIters = (targetLines >= 20000) ? 10 : 20;
    const size_t heavyIters = (targetLines >= 20000) ? 5 : 10;

    // 1. didOpen + diagnostics (distinct temporary URIs so each iteration runs cold open + analysis).
    {
        std::vector<double> samples;
        samples.reserve(heavyIters);
        const size_t rssBefore = angel_lsp::test::CurrentResidentBytes();
        for (size_t i = 0; i < heavyIters; ++i)
        {
            const std::string openUri = fixture.SandboxUri("bench_open_" + std::to_string(i) + ".as");
            angel_lsp::utils::HighResTimer timer;
            (void)session.OpenAndWait(openUri, script.source);
            samples.push_back(timer.ElapsedMs());
            session.SendDidClose(openUri);
        }
        const size_t rssAfter = angel_lsp::test::CurrentResidentBytes();
        PrintTableRow("didOpen + diagnostics", sizeLabel, samples, ComputeRssDeltaMib(rssBefore, rssAfter));
    }

    // Open main.as once for all subsequent feature queries.
    const lsp::json::Array mainDiags = session.OpenAndWait(mainUri, script.source);

    // 2. Hover (local variable, member, predefined symbol, included-file symbol).
    (void)BenchmarkRequest(session, "hover (local variable)", sizeLabel,
                           {"textDocument/hover", MakePositionParams(mainUri, script.localVarPos), fastIters});
    (void)BenchmarkRequest(session, "hover (member)", sizeLabel,
                           {"textDocument/hover", MakePositionParams(mainUri, script.memberPos), fastIters});
    (void)BenchmarkRequest(session, "hover (predefined symbol)", sizeLabel,
                           {"textDocument/hover", MakePositionParams(mainUri, script.predefPos), fastIters});
    (void)BenchmarkRequest(session, "hover (included file symbol)", sizeLabel,
                           {"textDocument/hover", MakePositionParams(mainUri, script.includedPos), fastIters});

    // 3. Navigation & symbol inspection.
    (void)BenchmarkRequest(session, "definition", sizeLabel,
                           {"textDocument/definition", MakePositionParams(mainUri, script.memberPos), fastIters});
    (void)BenchmarkRequest(session, "declaration", sizeLabel,
                           {"textDocument/declaration", MakePositionParams(mainUri, script.memberPos), fastIters});
    (void)BenchmarkRequest(session, "typeDefinition", sizeLabel,
                           {"textDocument/typeDefinition", MakePositionParams(mainUri, script.typeDefPos), fastIters});
    (void)BenchmarkRequest(session, "implementation", sizeLabel,
                           {"textDocument/implementation", MakePositionParams(mainUri, script.implPos), fastIters});
    (void)BenchmarkRequest(session, "moniker", sizeLabel,
                           {"textDocument/moniker", MakePositionParams(mainUri, script.callTargetPos), fastIters});
    {
        lsp::json::Value refParams = MakePositionParams(mainUri, script.localVarPos);
        lsp::json::Object refCtx;
        refCtx["includeDeclaration"] = lsp::json::Value(true);
        refParams.object()["context"] = lsp::json::Value(std::move(refCtx));
        (void)BenchmarkRequest(session, "references", sizeLabel,
                               {"textDocument/references", std::move(refParams), fastIters});
    }

    // 4. Rename.
    (void)BenchmarkRequest(session, "prepareRename", sizeLabel,
                           {"textDocument/prepareRename", MakePositionParams(mainUri, script.localVarPos), fastIters});
    {
        lsp::json::Value renParams = MakePositionParams(mainUri, script.localVarPos);
        renParams.object()["newName"] = lsp::json::Value(std::string("localAccumRenamed"));
        (void)BenchmarkRequest(session, "rename", sizeLabel, {"textDocument/rename", std::move(renParams), fastIters});
    }

    // 5. Completion & completionItem/resolve.
    (void)BenchmarkRequest(session, "completion (member .)", sizeLabel,
                           {"textDocument/completion", MakePositionParams(mainUri, script.memberDotPos), fastIters});
    const lsp::json::Value scopeCompResp =
        BenchmarkRequest(session, "completion (scope ::)", sizeLabel,
                         {"textDocument/completion", MakePositionParams(mainUri, script.scopeColonPos), fastIters});
    if (const auto item = PickCompletionItemForResolve(scopeCompResp); item.has_value())
    {
        (void)BenchmarkRequest(session, "completionItem/resolve", sizeLabel,
                               {"completionItem/resolve", *item, fastIters});
    }
    else
    {
        MESSAGE("NOTE: completionItem/resolve skipped for " << sizeLabel << " (no completion item returned)");
    }

    // 6. SignatureHelp.
    (void)BenchmarkRequest(session, "signatureHelp", sizeLabel,
                           {"textDocument/signatureHelp", MakePositionParams(mainUri, script.sigHelpPos), fastIters});

    // 7. SemanticTokens (full, range, delta).
    const lsp::json::Value fullTokResp =
        BenchmarkRequest(session, "semanticTokens/full", sizeLabel,
                         {"textDocument/semanticTokens/full", MakeTextDocParams(mainUri), heavyIters});
    (void)BenchmarkRequest(
        session, "semanticTokens/range", sizeLabel,
        {"textDocument/semanticTokens/range", MakeRangeParams(mainUri, script.formatRange), heavyIters});
    if (std::optional<std::string> prevId = ExtractSemanticResultId(fullTokResp); prevId.has_value())
    {
        std::vector<double> deltaSamples;
        deltaSamples.reserve(heavyIters);
        size_t tailLen = 23; // "// trailing edit anchor"
        const size_t rssBefore = angel_lsp::test::CurrentResidentBytes();
        for (size_t i = 0; i < heavyIters; ++i)
        {
            const lsp::Range editRange{lsp::Position{script.tailEditLine, 0},
                                       lsp::Position{script.tailEditLine, static_cast<uint32_t>(tailLen)}};
            const std::string lineText = "// trailing edit " + std::to_string(i);
            tailLen = lineText.size();
            (void)session.SendDidChangeRange(mainUri, editRange, lineText);

            lsp::json::Value deltaParams = MakeTextDocParams(mainUri);
            deltaParams.object()["previousResultId"] = lsp::json::Value(std::string(*prevId));
            angel_lsp::utils::HighResTimer timer;
            const lsp::json::Value deltaResp =
                session.Request("textDocument/semanticTokens/full/delta", std::move(deltaParams));
            deltaSamples.push_back(timer.ElapsedMs());
            if (auto nextId = ExtractSemanticResultId(deltaResp); nextId.has_value())
            {
                prevId = std::move(nextId);
            }
        }
        const size_t rssAfter = angel_lsp::test::CurrentResidentBytes();
        PrintTableRow("semanticTokens/full/delta", sizeLabel, deltaSamples, ComputeRssDeltaMib(rssBefore, rssAfter));
    }

    // 8. DocumentSymbol & Workspace/Symbol (+ resolve).
    (void)BenchmarkRequest(session, "documentSymbol", sizeLabel,
                           {"textDocument/documentSymbol", MakeTextDocParams(mainUri), fastIters});
    lsp::json::Object wsParams;
    wsParams["query"] = lsp::json::Value(std::string("Compute"));
    const lsp::json::Value wsResp = BenchmarkRequest(
        session, "workspace/symbol", sizeLabel, {"workspace/symbol", lsp::json::Value(std::move(wsParams)), fastIters});
    if (const auto wsSym = PickFirstArrayElement(wsResp); wsSym.has_value())
    {
        (void)BenchmarkRequest(session, "workspaceSymbol/resolve", sizeLabel,
                               {"workspaceSymbol/resolve", *wsSym, fastIters});
    }

    // 9. InlayHint & inlayHint/resolve.
    const lsp::Range fullDocRange{lsp::Position{0, 0}, lsp::Position{static_cast<uint32_t>(script.lineCount), 0}};
    const lsp::json::Value inlayResp =
        BenchmarkRequest(session, "inlayHint", sizeLabel,
                         {"textDocument/inlayHint", MakeRangeParams(mainUri, fullDocRange), heavyIters});
    if (const auto hint = PickFirstArrayElement(inlayResp); hint.has_value())
    {
        (void)BenchmarkRequest(session, "inlayHint/resolve", sizeLabel, {"inlayHint/resolve", *hint, fastIters});
    }
    else
    {
        MESSAGE("NOTE: inlayHint/resolve skipped for " << sizeLabel << " (no inlay hints returned)");
    }

    // 10. CodeLens & codeLens/resolve.
    const lsp::json::Value lensResp = BenchmarkRequest(
        session, "codeLens", sizeLabel, {"textDocument/codeLens", MakeTextDocParams(mainUri), heavyIters});
    if (const auto lens = PickFirstArrayElement(lensResp); lens.has_value())
    {
        (void)BenchmarkRequest(session, "codeLens/resolve", sizeLabel, {"codeLens/resolve", *lens, fastIters});
    }

    // 11. CodeAction & codeAction/resolve.
    {
        lsp::json::Value caParams = MakeRangeParams(mainUri, script.codeActionRange);
        lsp::json::Object caCtx;
        caCtx["diagnostics"] = lsp::json::Value(lsp::json::Array(mainDiags));
        caParams.object()["context"] = lsp::json::Value(std::move(caCtx));
        const lsp::json::Value caResp = BenchmarkRequest(session, "codeAction", sizeLabel,
                                                         {"textDocument/codeAction", std::move(caParams), heavyIters});
        if (const auto action = PickFirstArrayElement(caResp); action.has_value())
        {
            (void)BenchmarkRequest(session, "codeAction/resolve", sizeLabel,
                                   {"codeAction/resolve", *action, fastIters});
        }
        else
        {
            MESSAGE("NOTE: codeAction/resolve skipped for " << sizeLabel << " (no code actions returned)");
        }
    }

    // 12. DocumentLink & documentLink/resolve.
    const lsp::json::Value linkResp = BenchmarkRequest(
        session, "documentLink", sizeLabel, {"textDocument/documentLink", MakeTextDocParams(mainUri), fastIters});
    if (const auto link = PickFirstArrayElement(linkResp); link.has_value())
    {
        (void)BenchmarkRequest(session, "documentLink/resolve", sizeLabel, {"documentLink/resolve", *link, fastIters});
    }

    // 13. Formatting (document, range, ranges, onType).
    {
        lsp::json::Value docFmt = MakeTextDocParams(mainUri);
        docFmt.object()["options"] = MakeFormattingOptions();
        (void)BenchmarkRequest(session, "formatting (document)", sizeLabel,
                               {"textDocument/formatting", std::move(docFmt), heavyIters});

        lsp::json::Value rangeFmt = MakeRangeParams(mainUri, script.formatRange);
        rangeFmt.object()["options"] = MakeFormattingOptions();
        (void)BenchmarkRequest(session, "formatting (range)", sizeLabel,
                               {"textDocument/rangeFormatting", std::move(rangeFmt), heavyIters});

        lsp::json::Value rangesFmt = MakeTextDocParams(mainUri);
        lsp::json::Array rangesArr;
        rangesArr.push_back(ToJsonRange(script.formatRange));
        rangesFmt.object()["ranges"] = lsp::json::Value(std::move(rangesArr));
        rangesFmt.object()["options"] = MakeFormattingOptions();
        (void)BenchmarkRequest(session, "formatting (ranges)", sizeLabel,
                               {"textDocument/rangesFormatting", std::move(rangesFmt), heavyIters});

        lsp::json::Value onTypeFmt = MakePositionParams(mainUri, script.onTypePos);
        onTypeFmt.object()["ch"] = lsp::json::Value(std::string(";"));
        onTypeFmt.object()["options"] = MakeFormattingOptions();
        (void)BenchmarkRequest(session, "formatting (onType)", sizeLabel,
                               {"textDocument/onTypeFormatting", std::move(onTypeFmt), heavyIters});
    }

    // 14. FoldingRange, SelectionRange, DocumentHighlight, LinkedEditingRange.
    (void)BenchmarkRequest(session, "foldingRange", sizeLabel,
                           {"textDocument/foldingRange", MakeTextDocParams(mainUri), heavyIters});
    {
        lsp::json::Value selParams = MakeTextDocParams(mainUri);
        lsp::json::Array positions;
        positions.push_back(ToJsonPosition(script.localVarPos));
        selParams.object()["positions"] = lsp::json::Value(std::move(positions));
        (void)BenchmarkRequest(session, "selectionRange", sizeLabel,
                               {"textDocument/selectionRange", std::move(selParams), fastIters});
    }
    (void)BenchmarkRequest(
        session, "documentHighlight", sizeLabel,
        {"textDocument/documentHighlight", MakePositionParams(mainUri, script.localVarPos), fastIters});
    (void)BenchmarkRequest(
        session, "linkedEditingRange", sizeLabel,
        {"textDocument/linkedEditingRange", MakePositionParams(mainUri, script.localVarPos), fastIters});

    // 15. Call Hierarchy (prepare, incomingCalls, outgoingCalls).
    const lsp::json::Value prepCallTarget = BenchmarkRequest(
        session, "callHierarchy/prepare", sizeLabel,
        {"textDocument/prepareCallHierarchy", MakePositionParams(mainUri, script.callTargetPos), fastIters});
    if (const auto targetItem = PickFirstArrayElement(prepCallTarget); targetItem.has_value())
    {
        lsp::json::Object incParams;
        incParams["item"] = *targetItem;
        (void)BenchmarkRequest(session, "callHierarchy/incomingCalls", sizeLabel,
                               {"callHierarchy/incomingCalls", lsp::json::Value(std::move(incParams)), fastIters});
    }
    const lsp::json::Value prepCallWrapper =
        session.Request("textDocument/prepareCallHierarchy", MakePositionParams(mainUri, script.callWrapperPos));
    if (const auto wrapperItem = PickFirstArrayElement(prepCallWrapper); wrapperItem.has_value())
    {
        lsp::json::Object outParams;
        outParams["item"] = *wrapperItem;
        (void)BenchmarkRequest(session, "callHierarchy/outgoingCalls", sizeLabel,
                               {"callHierarchy/outgoingCalls", lsp::json::Value(std::move(outParams)), fastIters});
    }

    // 16. Type Hierarchy (prepare, supertypes, subtypes).
    const lsp::json::Value prepTypeBase = BenchmarkRequest(
        session, "typeHierarchy/prepare", sizeLabel,
        {"textDocument/prepareTypeHierarchy", MakePositionParams(mainUri, script.typeBasePos), fastIters});
    if (const auto baseItem = PickFirstArrayElement(prepTypeBase); baseItem.has_value())
    {
        lsp::json::Object subParams;
        subParams["item"] = *baseItem;
        (void)BenchmarkRequest(session, "typeHierarchy/subtypes", sizeLabel,
                               {"typeHierarchy/subtypes", lsp::json::Value(std::move(subParams)), fastIters});
    }
    const lsp::json::Value prepTypeDerived =
        session.Request("textDocument/prepareTypeHierarchy", MakePositionParams(mainUri, script.typeDerivedPos));
    if (const auto derivedItem = PickFirstArrayElement(prepTypeDerived); derivedItem.has_value())
    {
        lsp::json::Object supParams;
        supParams["item"] = *derivedItem;
        (void)BenchmarkRequest(session, "typeHierarchy/supertypes", sizeLabel,
                               {"typeHierarchy/supertypes", lsp::json::Value(std::move(supParams)), fastIters});
    }
}

/**
 * @brief Collects all `InlayHintLabelPart.location` targets from a `textDocument/inlayHint` response.
 */
struct InlayLocationTarget
{
    std::string uri;           ///< Target document URI from `location.uri`.
    lsp::Position position{};  ///< Target `location.range.start`.
    bool isPredefined = false; ///< True when `uri` points to a `.as.predefined` file.
};

/**
 * @brief Parses all `InlayHintLabelPart.location` entries from an inlayHint JSON-RPC response.
 * @param[in] inlayResp JSON-RPC response returned by `textDocument/inlayHint`.
 * @return List of discovered `InlayLocationTarget` records.
 */
inline std::vector<InlayLocationTarget> CollectInlayLocationTargets(const lsp::json::Value& inlayResp)
{
    std::vector<InlayLocationTarget> targets;
    const auto* res = ExtractResult(inlayResp);
    if (res == nullptr || !res->isArray())
    {
        return targets;
    }
    for (const auto& hint : res->array())
    {
        if (!hint.isObject())
        {
            continue;
        }
        const auto* labelVal = hint.object().find("label");
        if (labelVal == nullptr || !labelVal->isArray())
        {
            continue;
        }
        for (const auto& part : labelVal->array())
        {
            if (!part.isObject())
            {
                continue;
            }
            const auto* locVal = part.object().find("location");
            if (locVal == nullptr || !locVal->isObject())
            {
                continue;
            }
            const auto* uriVal = locVal->object().find("uri");
            const auto* rangeVal = locVal->object().find("range");
            if (uriVal == nullptr || !uriVal->isString() || rangeVal == nullptr || !rangeVal->isObject())
            {
                continue;
            }
            const auto* startVal = rangeVal->object().find("start");
            if (startVal == nullptr || !startVal->isObject())
            {
                continue;
            }
            const auto* lineVal = startVal->object().find("line");
            const auto* charVal = startVal->object().find("character");
            if (lineVal == nullptr || !lineVal->isInteger() || charVal == nullptr || !charVal->isInteger())
            {
                continue;
            }
            InlayLocationTarget target;
            target.uri = uriVal->string();
            target.position =
                lsp::Position{static_cast<lsp::uint>(lineVal->integer()), static_cast<lsp::uint>(charVal->integer())};
            target.isPredefined = angel_lsp::utils::IsPredefinedFile(target.uri, ".as.predefined");
            targets.push_back(std::move(target));
        }
    }
    return targets;
}

} // namespace feature_bench

TEST_SUITE("harness/FeatureBenchmark")
{
    TEST_CASE("FeatureBenchmark - Individual LSP Features (1k, 5k, 20k lines)" * doctest::skip(true))
    {
        MESSAGE("Seed: " << feature_bench::kBenchmarkSeed);
        feature_bench::PrintTableHeader();

        feature_bench::RunGeneratedScriptFeatureBenchmarks(1000, "1000 lines");
        feature_bench::RunGeneratedScriptFeatureBenchmarks(5000, "5000 lines");
        feature_bench::RunGeneratedScriptFeatureBenchmarks(20000, "20000 lines");
    }

    TEST_CASE("MemoryFootprintBenchmark - Retained containers at 20k lines" * doctest::skip(true))
    {
        MESSAGE("Seed: " << feature_bench::kBenchmarkSeed);
        std::mt19937_64 rng(feature_bench::kBenchmarkSeed);
        const feature_bench::GeneratedFeatureScript script = feature_bench::GenerateFeatureBenchmarkScript(20000, rng);

        angel_lsp::parser::AngelScriptParser parser;
        auto syntax = std::make_shared<angel_lsp::ParsedDocumentSyntax>(
            angel_lsp::parser::MakeTreePtr(parser.Parse(script.source)), script.source);
        REQUIRE(syntax->tree);

        angel_lsp::analysis::SymbolTable table;
        angel_lsp::analysis::SymbolCollector collector(nullptr);
        collector.CollectSymbolsWithIndex({"file:///footprint.as", script.source, nullptr}, syntax->index, table);

        angel_lsp::analysis::LocalScopeCollector scopes;
        const auto root = scopes.CollectScopesFromIndex(syntax->index, script.source);
        REQUIRE(root);
        size_t scopeBytes = 0;
        size_t scopeCount = 0;
        std::vector<const angel_lsp::analysis::Scope*> pending{root.get()};
        while (!pending.empty())
        {
            const auto* scope = pending.back();
            pending.pop_back();
            scopeBytes += scope->EstimateMemoryBytes();
            ++scopeCount;
            for (const auto& child : scope->children)
            {
                pending.push_back(child.get());
            }
        }

        constexpr double kMiB = 1024.0 * 1024.0;
        std::cout << std::fixed << std::setprecision(2) << "source        " << script.source.size() / kMiB << " MiB\n"
                  << "NodeIndex     " << syntax->index.EstimateMemoryBytes() / kMiB << " MiB ("
                  << syntax->index.NodeCount() << " nodes)\n"
                  << "tree (bound)  " << syntax->index.EstimateTreeBytes() / kMiB << " MiB\n"
                  << "ScopeTree     " << scopeBytes / kMiB << " MiB (" << scopeCount << " scopes)\n"
                  << "SymbolTable   " << table.EstimateMemoryBytes() / kMiB << " MiB\n";
    }

    TEST_CASE("FeatureBenchmark - Individual LSP Features (sdk-addons.as.predefined)" * doctest::skip(true))
    {
        feature_bench::PrintTableHeader();

        angel_lsp::test::LspSemanticHarnessFixture fixture;
        const std::string predefText = angel_lsp::test::LoadFixtureText("sdk-addons.as.predefined");
        REQUIRE(!predefText.empty());
        const size_t lineCount = static_cast<size_t>(std::count(predefText.begin(), predefText.end(), '\n') + 1);
        const std::string sizeLabel = "sdk-addons.as.predefined (" + std::to_string(lineCount) + " lines)";

        feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "sdk-addons.as.predefined", predefText);
        const std::string predefUri = fixture.SandboxUri("sdk-addons.as.predefined");
        const std::string rootUri = angel_lsp::utils::PathToUri(fixture.GetSandboxDir().string());

        angel_lsp::config::ServerConfig serverConfig;
        serverConfig.features.enableOnTypeFormatting = true;
        feature_bench::BenchmarkSession session(serverConfig, rootUri);

        constexpr size_t kFastIters = 20;
        constexpr size_t kHeavyIters = 10;

        // 1. didOpen + diagnostics on sdk-addons.as.predefined.
        {
            std::vector<double> samples;
            samples.reserve(kHeavyIters);
            const size_t rssBefore = angel_lsp::test::CurrentResidentBytes();
            for (size_t i = 0; i < kHeavyIters; ++i)
            {
                angel_lsp::utils::HighResTimer timer;
                (void)session.OpenAndWait(predefUri, predefText);
                samples.push_back(timer.ElapsedMs());
                if (i + 1 < kHeavyIters)
                {
                    session.SendDidClose(predefUri);
                }
            }
            const size_t rssAfter = angel_lsp::test::CurrentResidentBytes();
            feature_bench::PrintTableRow("didOpen + diagnostics", sizeLabel, samples,
                                         feature_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
        }

        const auto dictDeclPos = feature_bench::FindTokenPos(predefText, "dictionary{");
        const auto dictIterRefPos = feature_bench::FindTokenPos(predefText, "dictionaryIter@ opForBegin");
        const auto complexFieldPos = feature_bench::FindTokenPos(predefText, "float r;");
        const auto formatIntPos = feature_bench::FindTokenPos(predefText, "formatInt(");
        REQUIRE(dictDeclPos.has_value());
        REQUIRE(dictIterRefPos.has_value());
        REQUIRE(complexFieldPos.has_value());
        REQUIRE(formatIntPos.has_value());

        const lsp::Position memberFieldPos{complexFieldPos->line, complexFieldPos->character + 6}; // 'r'
        const lsp::Position memberSemiPos{complexFieldPos->line, complexFieldPos->character + 8};  // after ';'
        const lsp::Position formatIntNamePos = *formatIntPos;
        const lsp::Position formatIntArgPos{formatIntPos->line, formatIntPos->character + 10};
        const lsp::Range rangeSlice{lsp::Position{150, 0}, lsp::Position{200, 0}};

        // 2. Hover.
        (void)feature_bench::BenchmarkRequest(
            session, "hover (predefined class)", sizeLabel,
            {"textDocument/hover", feature_bench::MakePositionParams(predefUri, *dictDeclPos), kFastIters});
        (void)feature_bench::BenchmarkRequest(
            session, "hover (member)", sizeLabel,
            {"textDocument/hover", feature_bench::MakePositionParams(predefUri, memberFieldPos), kFastIters});
        (void)feature_bench::BenchmarkRequest(
            session, "hover (predefined function)", sizeLabel,
            {"textDocument/hover", feature_bench::MakePositionParams(predefUri, formatIntNamePos), kFastIters});

        // 3. Definition, declaration, references.
        (void)feature_bench::BenchmarkRequest(
            session, "definition", sizeLabel,
            {"textDocument/definition", feature_bench::MakePositionParams(predefUri, *dictIterRefPos), kFastIters});
        (void)feature_bench::BenchmarkRequest(
            session, "declaration", sizeLabel,
            {"textDocument/declaration", feature_bench::MakePositionParams(predefUri, *dictIterRefPos), kFastIters});
        {
            lsp::json::Value refParams = feature_bench::MakePositionParams(predefUri, *dictIterRefPos);
            lsp::json::Object refCtx;
            refCtx["includeDeclaration"] = lsp::json::Value(true);
            refParams.object()["context"] = lsp::json::Value(std::move(refCtx));
            (void)feature_bench::BenchmarkRequest(session, "references", sizeLabel,
                                                  {"textDocument/references", std::move(refParams), kFastIters});
        }

        // 4. Rename (expected null for predefined symbols, measures guard path).
        (void)feature_bench::BenchmarkRequest(
            session, "prepareRename", sizeLabel,
            {"textDocument/prepareRename", feature_bench::MakePositionParams(predefUri, *dictDeclPos), kFastIters});
        {
            lsp::json::Value renParams = feature_bench::MakePositionParams(predefUri, *dictDeclPos);
            renParams.object()["newName"] = lsp::json::Value(std::string("dictionaryRenamed"));
            (void)feature_bench::BenchmarkRequest(session, "rename", sizeLabel,
                                                  {"textDocument/rename", std::move(renParams), kFastIters});
        }

        // 5. Completion & completionItem/resolve.
        const lsp::json::Value compResp = feature_bench::BenchmarkRequest(
            session, "completion", sizeLabel,
            {"textDocument/completion", feature_bench::MakePositionParams(predefUri, formatIntNamePos), kFastIters});
        if (const auto compItem = feature_bench::PickCompletionItemForResolve(compResp); compItem.has_value())
        {
            (void)feature_bench::BenchmarkRequest(session, "completionItem/resolve", sizeLabel,
                                                  {"completionItem/resolve", *compItem, kFastIters});
        }

        // 6. SignatureHelp (declaration header only in .as.predefined, no call sites).
        (void)feature_bench::BenchmarkRequest(
            session, "signatureHelp", sizeLabel,
            {"textDocument/signatureHelp", feature_bench::MakePositionParams(predefUri, formatIntArgPos), kFastIters});

        // 7. SemanticTokens full, range, delta.
        const lsp::json::Value fullTokResp = feature_bench::BenchmarkRequest(
            session, "semanticTokens/full", sizeLabel,
            {"textDocument/semanticTokens/full", feature_bench::MakeTextDocParams(predefUri), kHeavyIters});
        (void)feature_bench::BenchmarkRequest(
            session, "semanticTokens/range", sizeLabel,
            {"textDocument/semanticTokens/range", feature_bench::MakeRangeParams(predefUri, rangeSlice), kHeavyIters});
        if (const auto prevId = feature_bench::ExtractSemanticResultId(fullTokResp); prevId.has_value())
        {
            lsp::json::Value deltaParams = feature_bench::MakeTextDocParams(predefUri);
            deltaParams.object()["previousResultId"] = lsp::json::Value(std::string(*prevId));
            (void)feature_bench::BenchmarkRequest(
                session, "semanticTokens/full/delta", sizeLabel,
                {"textDocument/semanticTokens/full/delta", std::move(deltaParams), kHeavyIters});
        }

        // 8. DocumentSymbol & Workspace/Symbol.
        (void)feature_bench::BenchmarkRequest(
            session, "documentSymbol", sizeLabel,
            {"textDocument/documentSymbol", feature_bench::MakeTextDocParams(predefUri), kFastIters});
        {
            lsp::json::Object wsParams;
            wsParams["query"] = lsp::json::Value(std::string("dictionary"));
            (void)feature_bench::BenchmarkRequest(
                session, "workspace/symbol", sizeLabel,
                {"workspace/symbol", lsp::json::Value(std::move(wsParams)), kFastIters});
        }

        // 9. InlayHint (intentionally empty on .as.predefined).
        const lsp::Range fullRange{lsp::Position{0, 0}, lsp::Position{static_cast<uint32_t>(lineCount), 0}};
        const lsp::json::Value inlayResp = feature_bench::BenchmarkRequest(
            session, "inlayHint", sizeLabel,
            {"textDocument/inlayHint", feature_bench::MakeRangeParams(predefUri, fullRange), kHeavyIters});
        if (const auto hint = feature_bench::PickFirstArrayElement(inlayResp); hint.has_value())
        {
            (void)feature_bench::BenchmarkRequest(session, "inlayHint/resolve", sizeLabel,
                                                  {"inlayHint/resolve", *hint, kFastIters});
        }
        else
        {
            MESSAGE("NOTE: inlayHint/resolve unreachable on " << sizeLabel
                                                              << " (HandleRequestsTextDocument_InlayHint returns [])");
        }

        // 10. CodeLens & resolve.
        const lsp::json::Value lensResp = feature_bench::BenchmarkRequest(
            session, "codeLens", sizeLabel,
            {"textDocument/codeLens", feature_bench::MakeTextDocParams(predefUri), kHeavyIters});
        if (const auto lens = feature_bench::PickFirstArrayElement(lensResp); lens.has_value())
        {
            (void)feature_bench::BenchmarkRequest(session, "codeLens/resolve", sizeLabel,
                                                  {"codeLens/resolve", *lens, kFastIters});
        }

        // 11. CodeAction & resolve.
        {
            lsp::json::Value caParams =
                feature_bench::MakeRangeParams(predefUri, lsp::Range{*complexFieldPos, memberSemiPos});
            lsp::json::Object caCtx;
            caCtx["diagnostics"] = lsp::json::Value(lsp::json::Array{});
            caParams.object()["context"] = lsp::json::Value(std::move(caCtx));
            const lsp::json::Value caResp = feature_bench::BenchmarkRequest(
                session, "codeAction", sizeLabel, {"textDocument/codeAction", std::move(caParams), kHeavyIters});
            if (const auto action = feature_bench::PickFirstArrayElement(caResp); action.has_value())
            {
                (void)feature_bench::BenchmarkRequest(session, "codeAction/resolve", sizeLabel,
                                                      {"codeAction/resolve", *action, kFastIters});
            }
            else
            {
                MESSAGE("NOTE: codeAction/resolve skipped on " << sizeLabel << " (no actions emitted)");
            }
        }

        // 12. DocumentLink & resolve (no #include directives in sdk-addons.as.predefined).
        const lsp::json::Value linkResp = feature_bench::BenchmarkRequest(
            session, "documentLink", sizeLabel,
            {"textDocument/documentLink", feature_bench::MakeTextDocParams(predefUri), kFastIters});
        if (const auto link = feature_bench::PickFirstArrayElement(linkResp); link.has_value())
        {
            (void)feature_bench::BenchmarkRequest(session, "documentLink/resolve", sizeLabel,
                                                  {"documentLink/resolve", *link, kFastIters});
        }
        else
        {
            MESSAGE("NOTE: documentLink/resolve unreachable on " << sizeLabel << " (no #include directives)");
        }

        // 13. Formatting (document, range, onType).
        {
            lsp::json::Value docFmt = feature_bench::MakeTextDocParams(predefUri);
            docFmt.object()["options"] = feature_bench::MakeFormattingOptions();
            (void)feature_bench::BenchmarkRequest(session, "formatting (document)", sizeLabel,
                                                  {"textDocument/formatting", std::move(docFmt), kHeavyIters});

            lsp::json::Value rangeFmt = feature_bench::MakeRangeParams(predefUri, rangeSlice);
            rangeFmt.object()["options"] = feature_bench::MakeFormattingOptions();
            (void)feature_bench::BenchmarkRequest(session, "formatting (range)", sizeLabel,
                                                  {"textDocument/rangeFormatting", std::move(rangeFmt), kHeavyIters});

            lsp::json::Value onTypeFmt = feature_bench::MakePositionParams(predefUri, memberSemiPos);
            onTypeFmt.object()["ch"] = lsp::json::Value(std::string(";"));
            onTypeFmt.object()["options"] = feature_bench::MakeFormattingOptions();
            (void)feature_bench::BenchmarkRequest(session, "formatting (onType)", sizeLabel,
                                                  {"textDocument/onTypeFormatting", std::move(onTypeFmt), kHeavyIters});
        }

        // 14. FoldingRange, SelectionRange, DocumentHighlight.
        (void)feature_bench::BenchmarkRequest(
            session, "foldingRange", sizeLabel,
            {"textDocument/foldingRange", feature_bench::MakeTextDocParams(predefUri), kHeavyIters});
        {
            lsp::json::Value selParams = feature_bench::MakeTextDocParams(predefUri);
            lsp::json::Array positions;
            positions.push_back(feature_bench::ToJsonPosition(*dictIterRefPos));
            selParams.object()["positions"] = lsp::json::Value(std::move(positions));
            (void)feature_bench::BenchmarkRequest(session, "selectionRange", sizeLabel,
                                                  {"textDocument/selectionRange", std::move(selParams), kFastIters});
        }
        (void)feature_bench::BenchmarkRequest(session, "documentHighlight", sizeLabel,
                                              {"textDocument/documentHighlight",
                                               feature_bench::MakePositionParams(predefUri, *dictIterRefPos),
                                               kFastIters});

        // 15. Call Hierarchy & Type Hierarchy.
        const lsp::json::Value prepCall = feature_bench::BenchmarkRequest(
            session, "callHierarchy/prepare", sizeLabel,
            {"textDocument/prepareCallHierarchy", feature_bench::MakePositionParams(predefUri, formatIntNamePos),
             kFastIters});
        if (const auto callItem = feature_bench::PickFirstArrayElement(prepCall); callItem.has_value())
        {
            lsp::json::Object incParams;
            incParams["item"] = *callItem;
            (void)feature_bench::BenchmarkRequest(
                session, "callHierarchy/incomingCalls", sizeLabel,
                {"callHierarchy/incomingCalls", lsp::json::Value(std::move(incParams)), kFastIters});
        }

        const lsp::json::Value prepType =
            feature_bench::BenchmarkRequest(session, "typeHierarchy/prepare", sizeLabel,
                                            {"textDocument/prepareTypeHierarchy",
                                             feature_bench::MakePositionParams(predefUri, *dictDeclPos), kFastIters});
        if (const auto typeItem = feature_bench::PickFirstArrayElement(prepType); typeItem.has_value())
        {
            lsp::json::Object supParams;
            supParams["item"] = *typeItem;
            (void)feature_bench::BenchmarkRequest(
                session, "typeHierarchy/supertypes", sizeLabel,
                {"typeHierarchy/supertypes", lsp::json::Value(std::move(supParams)), kFastIters});
            lsp::json::Object subParams;
            subParams["item"] = *typeItem;
            (void)feature_bench::BenchmarkRequest(
                session, "typeHierarchy/subtypes", sizeLabel,
                {"typeHierarchy/subtypes", lsp::json::Value(std::move(subParams)), kFastIters});
        }
    }

    TEST_CASE("FeatureBenchmark - Combined Flow 1: InlayHint Locations -> Hover (Not Open vs Already Open)" *
              doctest::skip(true))
    {
        feature_bench::PrintTableHeader();

        angel_lsp::test::LspSemanticHarnessFixture fixture;
        const std::string predefText = angel_lsp::test::LoadFixtureText("sdk-addons.as.predefined");
        REQUIRE(!predefText.empty());
        const std::string includedText = feature_bench::BuildIncludedLibrarySource();

        feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "sdk-addons.as.predefined", predefText);
        feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "included_lib.as", includedText);

        std::mt19937_64 rng(feature_bench::kBenchmarkSeed);
        const feature_bench::GeneratedFeatureScript script = feature_bench::GenerateFeatureBenchmarkScript(5000, rng);
        feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "main.as", script.source);

        const std::string mainUri = fixture.SandboxUri("main.as");
        const std::string predefUri = fixture.SandboxUri("sdk-addons.as.predefined");
        const std::string includedUri = fixture.SandboxUri("included_lib.as");
        const std::string rootUri = angel_lsp::utils::PathToUri(fixture.GetSandboxDir().string());

        angel_lsp::config::ServerConfig serverConfig;
        feature_bench::BenchmarkSession session(serverConfig, rootUri);
        (void)session.OpenAndWait(mainUri, script.source);

        const lsp::Range fullRange{lsp::Position{0, 0}, lsp::Position{static_cast<uint32_t>(script.lineCount), 0}};
        constexpr size_t kIterations = 5;

        auto textForTargetUri = [&](std::string_view uri) -> const std::string&
        {
            const std::string key = angel_lsp::Server::DocumentKey(uri);
            if (key == angel_lsp::Server::DocumentKey(predefUri))
            {
                return predefText;
            }
            if (key == angel_lsp::Server::DocumentKey(includedUri))
            {
                return includedText;
            }
            return script.source;
        };

        // Pass A: target documents NOT opened yet (VS Code opens target via didOpen, then hovers at location).
        std::vector<double> inlayNotOpenSamples;
        std::vector<double> predefDidOpenSamples;
        std::vector<double> predefHoverNotOpenSamples;
        std::vector<double> includedDidOpenSamples;
        std::vector<double> includedHoverNotOpenSamples;

        const size_t passARssBefore = angel_lsp::test::CurrentResidentBytes();
        for (size_t iter = 0; iter < kIterations; ++iter)
        {
            angel_lsp::utils::HighResTimer inlayTimer;
            const lsp::json::Value inlayResp =
                session.Request("textDocument/inlayHint", feature_bench::MakeRangeParams(mainUri, fullRange));
            inlayNotOpenSamples.push_back(inlayTimer.ElapsedMs());

            const auto targets = feature_bench::CollectInlayLocationTargets(inlayResp);
            bool sawPredef = false;
            bool sawIncluded = false;
            angel_lsp::TransparentStringSet openedThisPass;

            for (const auto& target : targets)
            {
                const std::string key = angel_lsp::Server::DocumentKey(target.uri);
                const bool isPredef = target.isPredefined;
                const bool isIncluded = (!isPredef && key == angel_lsp::Server::DocumentKey(includedUri));
                if (!isPredef && !isIncluded)
                {
                    continue;
                }

                if (!openedThisPass.contains(key))
                {
                    openedThisPass.insert(key);
                    const std::string& content = textForTargetUri(target.uri);
                    angel_lsp::utils::HighResTimer openTimer;
                    (void)session.OpenAndWait(target.uri, content);
                    const double openMs = openTimer.ElapsedMs();
                    if (isPredef)
                    {
                        predefDidOpenSamples.push_back(openMs);
                    }
                    else
                    {
                        includedDidOpenSamples.push_back(openMs);
                    }
                }

                angel_lsp::utils::HighResTimer hoverTimer;
                (void)session.Request("textDocument/hover",
                                      feature_bench::MakePositionParams(target.uri, target.position));
                const double hoverMs = hoverTimer.ElapsedMs();
                if (isPredef)
                {
                    sawPredef = true;
                    predefHoverNotOpenSamples.push_back(hoverMs);
                }
                else
                {
                    sawIncluded = true;
                    includedHoverNotOpenSamples.push_back(hoverMs);
                }
            }

            REQUIRE(sawPredef);
            REQUIRE(sawIncluded);

            if (iter + 1 < kIterations)
            {
                for (const std::string& openKey : openedThisPass)
                {
                    session.SendDidClose(openKey);
                }
            }
        }
        const size_t passARssAfter = angel_lsp::test::CurrentResidentBytes();
        const double passARssDelta = feature_bench::ComputeRssDeltaMib(passARssBefore, passARssAfter);

        feature_bench::PrintTableRow("flow1: inlayHint (before not-open)", "5000 lines", inlayNotOpenSamples,
                                     passARssDelta);
        feature_bench::PrintTableRow("flow1 (not-open): didOpen [predefined target]", "sdk-addons.as.predefined",
                                     predefDidOpenSamples, passARssDelta);
        feature_bench::PrintTableRow("flow1 (not-open): hover [predefined target]", "sdk-addons.as.predefined",
                                     predefHoverNotOpenSamples, passARssDelta);
        feature_bench::PrintTableRow("flow1 (not-open): didOpen [included target]", "included_lib.as",
                                     includedDidOpenSamples, passARssDelta);
        feature_bench::PrintTableRow("flow1 (not-open): hover [included target]", "included_lib.as",
                                     includedHoverNotOpenSamples, passARssDelta);

        // Pass B: target documents ALREADY open (hover directly at every inlay label part location).
        std::vector<double> inlayOpenSamples;
        std::vector<double> predefHoverOpenSamples;
        std::vector<double> includedHoverOpenSamples;

        const size_t passBRssBefore = angel_lsp::test::CurrentResidentBytes();
        for (size_t iter = 0; iter < kIterations; ++iter)
        {
            angel_lsp::utils::HighResTimer inlayTimer;
            const lsp::json::Value inlayResp =
                session.Request("textDocument/inlayHint", feature_bench::MakeRangeParams(mainUri, fullRange));
            inlayOpenSamples.push_back(inlayTimer.ElapsedMs());

            const auto targets = feature_bench::CollectInlayLocationTargets(inlayResp);
            for (const auto& target : targets)
            {
                const std::string key = angel_lsp::Server::DocumentKey(target.uri);
                const bool isPredef = target.isPredefined;
                const bool isIncluded = (!isPredef && key == angel_lsp::Server::DocumentKey(includedUri));
                if (!isPredef && !isIncluded)
                {
                    continue;
                }

                angel_lsp::utils::HighResTimer hoverTimer;
                (void)session.Request("textDocument/hover",
                                      feature_bench::MakePositionParams(target.uri, target.position));
                const double hoverMs = hoverTimer.ElapsedMs();
                if (isPredef)
                {
                    predefHoverOpenSamples.push_back(hoverMs);
                }
                else
                {
                    includedHoverOpenSamples.push_back(hoverMs);
                }
            }
        }
        const size_t passBRssAfter = angel_lsp::test::CurrentResidentBytes();
        const double passBRssDelta = feature_bench::ComputeRssDeltaMib(passBRssBefore, passBRssAfter);

        REQUIRE(!predefHoverOpenSamples.empty());
        REQUIRE(!includedHoverOpenSamples.empty());

        feature_bench::PrintTableRow("flow1: inlayHint (already-open)", "5000 lines", inlayOpenSamples, passBRssDelta);
        feature_bench::PrintTableRow("flow1 (already-open): hover [predefined target]", "sdk-addons.as.predefined",
                                     predefHoverOpenSamples, passBRssDelta);
        feature_bench::PrintTableRow("flow1 (already-open): hover [included target]", "included_lib.as",
                                     includedHoverOpenSamples, passBRssDelta);
    }

    TEST_CASE("FeatureBenchmark - Combined Flow 2: Burst Edits -> Diagnostics, InlayHint, SemanticTokens" *
              doctest::skip(true))
    {
        feature_bench::PrintTableHeader();

        struct BurstSpec
        {
            size_t targetLines;
            size_t bursts;
            const char* label;
        };
        const BurstSpec specs[] = {
            {5000, 10, "5000 lines"},
            {20000, 5, "20000 lines"},
        };

        for (const BurstSpec& spec : specs)
        {
            angel_lsp::test::LspSemanticHarnessFixture fixture;
            const std::string predefText = angel_lsp::test::LoadFixtureText("sdk-addons.as.predefined");
            feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "sdk-addons.as.predefined", predefText);
            feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "included_lib.as",
                                            feature_bench::BuildIncludedLibrarySource());

            std::mt19937_64 rng(feature_bench::kBenchmarkSeed + spec.targetLines);
            const feature_bench::GeneratedFeatureScript script =
                feature_bench::GenerateFeatureBenchmarkScript(spec.targetLines, rng);
            feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "main.as", script.source);

            const std::string mainUri = fixture.SandboxUri("main.as");
            const std::string rootUri = angel_lsp::utils::PathToUri(fixture.GetSandboxDir().string());

            angel_lsp::config::ServerConfig serverConfig;
            feature_bench::BenchmarkSession session(serverConfig, rootUri);
            (void)session.OpenAndWait(mainUri, script.source);

            const lsp::Range fullRange{lsp::Position{0, 0}, lsp::Position{static_cast<uint32_t>(script.lineCount), 0}};

            std::vector<double> burstDiagSamples;
            std::vector<double> inlaySamples;
            std::vector<double> semTokSamples;
            std::vector<double> totalSamples;
            burstDiagSamples.reserve(spec.bursts);
            inlaySamples.reserve(spec.bursts);
            semTokSamples.reserve(spec.bursts);
            totalSamples.reserve(spec.bursts);

            size_t tailLen = 23; // "// trailing edit anchor"
            const size_t rssBefore = angel_lsp::test::CurrentResidentBytes();
            for (size_t burst = 0; burst < spec.bursts; ++burst)
            {
                constexpr int kBurstSize = 10;
                angel_lsp::utils::HighResTimer totalTimer;
                angel_lsp::utils::HighResTimer diagTimer;
                int lastVer = 0;
                for (int step = 0; step < kBurstSize; ++step)
                {
                    const lsp::Range editRange{lsp::Position{script.tailEditLine, 0},
                                               lsp::Position{script.tailEditLine, static_cast<uint32_t>(tailLen)}};
                    const std::string text = "// burst_" + std::to_string(burst) + "_" + std::to_string(step);
                    tailLen = text.size();
                    lastVer = session.SendDidChangeRange(mainUri, editRange, text);
                }
                (void)session.WaitForPublished(mainUri, lastVer);
                burstDiagSamples.push_back(diagTimer.ElapsedMs());

                angel_lsp::utils::HighResTimer inlayTimer;
                (void)session.Request("textDocument/inlayHint", feature_bench::MakeRangeParams(mainUri, fullRange));
                inlaySamples.push_back(inlayTimer.ElapsedMs());

                angel_lsp::utils::HighResTimer tokTimer;
                (void)session.Request("textDocument/semanticTokens/full", feature_bench::MakeTextDocParams(mainUri));
                semTokSamples.push_back(tokTimer.ElapsedMs());

                totalSamples.push_back(totalTimer.ElapsedMs());
            }
            const size_t rssAfter = angel_lsp::test::CurrentResidentBytes();
            const double rssDelta = feature_bench::ComputeRssDeltaMib(rssBefore, rssAfter);

            feature_bench::PrintTableRow("flow2: burst 10 didChange -> diagnostics", spec.label, burstDiagSamples,
                                         rssDelta);
            feature_bench::PrintTableRow("flow2: post-burst inlayHint", spec.label, inlaySamples, rssDelta);
            feature_bench::PrintTableRow("flow2: post-burst semanticTokens/full", spec.label, semTokSamples, rssDelta);
            feature_bench::PrintTableRow("flow2: burst total (diags+inlay+semTok)", spec.label, totalSamples, rssDelta);
        }
    }

    TEST_CASE("FeatureBenchmark - Combined Flow 3: Character-by-Character Typing Completion" * doctest::skip(true))
    {
        feature_bench::PrintTableHeader();

        struct TypingSpec
        {
            size_t targetLines;
            size_t rounds;
            const char* label;
        };
        const TypingSpec specs[] = {
            {5000, 3, "5000 lines"},
            {20000, 1, "20000 lines"},
        };

        constexpr std::string_view kTypedIdentifier = "m_probeField";

        for (const TypingSpec& spec : specs)
        {
            angel_lsp::test::LspSemanticHarnessFixture fixture;
            const std::string predefText = angel_lsp::test::LoadFixtureText("sdk-addons.as.predefined");
            feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "sdk-addons.as.predefined", predefText);
            feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "included_lib.as",
                                            feature_bench::BuildIncludedLibrarySource());

            std::mt19937_64 rng(feature_bench::kBenchmarkSeed + spec.targetLines);
            const feature_bench::GeneratedFeatureScript script =
                feature_bench::GenerateFeatureBenchmarkScript(spec.targetLines, rng);
            feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "main.as", script.source);

            const std::string mainUri = fixture.SandboxUri("main.as");
            const std::string rootUri = angel_lsp::utils::PathToUri(fixture.GetSandboxDir().string());

            angel_lsp::config::ServerConfig serverConfig;
            feature_bench::BenchmarkSession session(serverConfig, rootUri);
            (void)session.OpenAndWait(mainUri, script.source);

            std::vector<double> completionOnlySamples;
            std::vector<double> keystrokeTotalSamples;
            completionOnlySamples.reserve(spec.rounds * kTypedIdentifier.size());
            keystrokeTotalSamples.reserve(spec.rounds * kTypedIdentifier.size());

            const size_t rssBefore = angel_lsp::test::CurrentResidentBytes();
            for (size_t round = 0; round < spec.rounds; ++round)
            {
                for (size_t idx = 0; idx < kTypedIdentifier.size(); ++idx)
                {
                    const uint32_t col = script.typingDotPos.character + static_cast<uint32_t>(idx);
                    const lsp::Range insertRange{lsp::Position{script.typingDotPos.line, col},
                                                 lsp::Position{script.typingDotPos.line, col}};
                    const std::string ch(1, kTypedIdentifier[idx]);

                    angel_lsp::utils::HighResTimer keyTimer;
                    (void)session.SendDidChangeRange(mainUri, insertRange, ch);

                    angel_lsp::utils::HighResTimer compTimer;
                    const lsp::json::Value compResp = session.Request(
                        "textDocument/completion",
                        feature_bench::MakePositionParams(mainUri, lsp::Position{script.typingDotPos.line, col + 1}));
                    completionOnlySamples.push_back(compTimer.ElapsedMs());
                    keystrokeTotalSamples.push_back(keyTimer.ElapsedMs());

                    REQUIRE(feature_bench::ExtractResult(compResp) != nullptr);
                }

                // Clear typed identifier back to `typeObj.` before next round.
                const lsp::Range resetRange{
                    script.typingDotPos,
                    lsp::Position{script.typingDotPos.line,
                                  script.typingDotPos.character + static_cast<uint32_t>(kTypedIdentifier.size())}};
                const int resetVer = session.SendDidChangeRange(mainUri, resetRange, "");
                (void)session.WaitForPublished(mainUri, resetVer);
            }
            const size_t rssAfter = angel_lsp::test::CurrentResidentBytes();
            const double rssDelta = feature_bench::ComputeRssDeltaMib(rssBefore, rssAfter);

            feature_bench::PrintTableRow("flow3: per-char completion", spec.label, completionOnlySamples, rssDelta);
            feature_bench::PrintTableRow("flow3: per-char didChange+completion", spec.label, keystrokeTotalSamples,
                                         rssDelta);
        }
    }

    TEST_CASE("FeatureBenchmark - Combined Flow 4: Hover and SignatureHelp During In-Flight Analysis" *
              doctest::skip(true))
    {
        feature_bench::PrintTableHeader();

        angel_lsp::test::LspSemanticHarnessFixture fixture;
        const std::string predefText = angel_lsp::test::LoadFixtureText("sdk-addons.as.predefined");
        feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "sdk-addons.as.predefined", predefText);
        feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "included_lib.as",
                                        feature_bench::BuildIncludedLibrarySource());

        std::mt19937_64 rng(feature_bench::kBenchmarkSeed);
        const feature_bench::GeneratedFeatureScript script = feature_bench::GenerateFeatureBenchmarkScript(5000, rng);
        feature_bench::WriteSandboxFile(fixture.GetSandboxDir(), "main.as", script.source);

        const std::string mainUri = fixture.SandboxUri("main.as");
        const std::string rootUri = angel_lsp::utils::PathToUri(fixture.GetSandboxDir().string());

        angel_lsp::config::ServerConfig serverConfig;
        feature_bench::BenchmarkSession session(serverConfig, rootUri);
        (void)session.OpenAndWait(mainUri, script.source);

        std::mutex hookMutex;
        std::condition_variable hookCv;
        int pausedTargetVersion = -1;
        bool hookReached = false;
        bool allowCommit = true;

        session.ServerRef().SetOnBeforeCommitHook(
            [&]([[maybe_unused]] std::string_view uri, int version, [[maybe_unused]] uint64_t generation)
            {
                std::unique_lock<std::mutex> lock(hookMutex);
                if (version == pausedTargetVersion)
                {
                    hookReached = true;
                    hookCv.notify_all();
                    hookCv.wait(lock, [&]() { return allowCommit; });
                }
            });

        constexpr size_t kIterations = 10;
        std::vector<double> hoverInFlightSamples;
        std::vector<double> sigHelpInFlightSamples;
        hoverInFlightSamples.reserve(kIterations);
        sigHelpInFlightSamples.reserve(kIterations);

        size_t tailLen = 23; // "// trailing edit anchor"
        const size_t rssBefore = angel_lsp::test::CurrentResidentBytes();
        for (size_t iter = 0; iter < kIterations; ++iter)
        {
            const lsp::Range editRange{lsp::Position{script.tailEditLine, 0},
                                       lsp::Position{script.tailEditLine, static_cast<uint32_t>(tailLen)}};
            const std::string text = "// inflight_iter_" + std::to_string(iter);
            tailLen = text.size();

            {
                std::lock_guard<std::mutex> lock(hookMutex);
                hookReached = false;
                allowCommit = false;
                // Predict next version without consuming NextVersion() before SendDidChangeRange().
                pausedTargetVersion = static_cast<int>(iter) + 2;
            }

            const int sentVersion = session.SendDidChangeRange(mainUri, editRange, text);
            REQUIRE(sentVersion == pausedTargetVersion);

            {
                std::unique_lock<std::mutex> lock(hookMutex);
                hookCv.wait(lock, [&]() { return hookReached; });
            }

            // Worker thread is deterministically held mid-analysis before commit; issue hover and signatureHelp now.
            {
                angel_lsp::utils::HighResTimer timer;
                const lsp::json::Value hoverResp = session.Request(
                    "textDocument/hover", feature_bench::MakePositionParams(mainUri, script.localVarPos));
                hoverInFlightSamples.push_back(timer.ElapsedMs());
                REQUIRE(feature_bench::ExtractResult(hoverResp) != nullptr);
            }
            {
                angel_lsp::utils::HighResTimer timer;
                const lsp::json::Value sigResp = session.Request(
                    "textDocument/signatureHelp", feature_bench::MakePositionParams(mainUri, script.sigHelpPos));
                sigHelpInFlightSamples.push_back(timer.ElapsedMs());
                REQUIRE(feature_bench::ExtractResult(sigResp) != nullptr);
            }

            {
                std::lock_guard<std::mutex> lock(hookMutex);
                allowCommit = true;
            }
            hookCv.notify_all();
            (void)session.WaitForPublished(mainUri, sentVersion);
        }
        const size_t rssAfter = angel_lsp::test::CurrentResidentBytes();
        const double rssDelta = feature_bench::ComputeRssDeltaMib(rssBefore, rssAfter);

        session.ServerRef().SetOnBeforeCommitHook(nullptr);

        feature_bench::PrintTableRow("flow4: hover (analysis in flight)", "5000 lines", hoverInFlightSamples, rssDelta);
        feature_bench::PrintTableRow("flow4: signatureHelp (analysis in flight)", "5000 lines", sigHelpInFlightSamples,
                                     rssDelta);
    }
}
