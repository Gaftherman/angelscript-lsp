#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>

namespace angel_lsp::utils
{
/**
 * @brief Discrete functional channels for isolated multi-file disk logging.
 */
enum class LogChannel
{
    Master,
    Hover,
    Analysis,
    Symbols,
    Crash
};

/**
 * @brief Severity level for multi-file log entries.
 */
enum class MultiFileLogLevel
{
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

/**
 * @brief High-performance asynchronous disk logger routing entries to isolated feature logs.
 */
class MultiFileLogger
{
public:
    MultiFileLogger();
    explicit MultiFileLogger(const std::filesystem::path& logDirectory);
    ~MultiFileLogger();

    MultiFileLogger(const MultiFileLogger&) = delete;
    MultiFileLogger& operator=(const MultiFileLogger&) = delete;

    /**
     * @brief Initializes or reconfigures the destination log directory.
     * @param[in] baseDirectory Base directory, e.g. workspace root or .vscode/lsp.
     */
    void Initialize(const std::filesystem::path& baseDirectory);

    /**
     * @brief Logs a message to the specified feature channel and mirrors to master.
     * @param[in] channel Target feature channel sink.
     * @param[in] level Severity level.
     * @param[in] message Log body.
     * @param[in] durationMs Optional elapsed operation duration in milliseconds (-1.0 if none).
     */
    void Log(LogChannel channel, MultiFileLogLevel level, std::string_view message, double durationMs = -1.0);

    /**
     * @brief Convenience helper to log to Master channel.
     * @param[in] level Severity level.
     * @param[in] message Log body.
     */
    void LogMaster(MultiFileLogLevel level, std::string_view message);

    /**
     * @brief Convenience helper to log to Hover channel.
     * @param[in] level Severity level.
     * @param[in] message Log body.
     * @param[in] durationMs Elapsed duration in milliseconds.
     */
    void LogHover(MultiFileLogLevel level, std::string_view message, double durationMs = -1.0);

    /**
     * @brief Convenience helper to log to Analysis channel.
     * @param[in] level Severity level.
     * @param[in] message Log body.
     * @param[in] durationMs Elapsed duration in milliseconds.
     */
    void LogAnalysis(MultiFileLogLevel level, std::string_view message, double durationMs = -1.0);

    /**
     * @brief Convenience helper to log to Symbols channel.
     * @param[in] level Severity level.
     * @param[in] message Log body.
     * @param[in] durationMs Elapsed duration in milliseconds.
     */
    void LogSymbols(MultiFileLogLevel level, std::string_view message, double durationMs = -1.0);

    /**
     * @brief Convenience helper to log emergency crash/panic dumps with immediate flush.
     * @param[in] message Crash summary or panic traceback.
     */
    void LogCrash(std::string_view message);

    /**
     * @brief Flushes all pending log entries to disk immediately across all channels.
     */
    void Flush();

    /**
     * @brief Returns the resolved active directory containing date-stamped log files.
     * @return Path to directory containing master.log, hover.log, etc.
     */
    [[nodiscard]] std::filesystem::path GetActiveLogDirectory() const;

    /**
     * @brief Checks whether the logger is actively configured and logging to disk.
     * @return True if initialized with a valid writable directory.
     */
    [[nodiscard]] bool IsInitialized() const;

    /**
     * @brief Formats a millisecond-accurate timestamp string for the current system time.
     * @param[in] time System clock time point.
     * @return Formatted timestamp: "YYYY-MM-DD HH:mm:ss.mmm".
     */
    static std::string FormatTimestamp(std::chrono::system_clock::time_point time);

    /**
     * @brief Returns the singleton instance of the MultiFileLogger.
     * @return Reference to global MultiFileLogger.
     */
    static MultiFileLogger& Instance();

private:
    struct LogEntry
    {
        LogChannel channel;
        MultiFileLogLevel level;
        std::chrono::system_clock::time_point timestamp;
        uint64_t threadId = 0;
        std::string message;
        double durationMs = -1.0;
    };

    void WorkerLoop();
    void ProcessBatch(std::vector<LogEntry>& batch);
    void WriteEntry(const LogEntry& entry);
    void FlushAllSinks();
    void EnsureSinksOpen();
    void CloseSinks();
    std::ofstream* GetChannelSink(LogChannel channel);
    static std::string FormatLogLine(const LogEntry& entry);

    std::filesystem::path m_logDirectory;
    mutable std::mutex m_mutex;
    mutable std::mutex m_sinkMutex;
    std::condition_variable m_cv;
    std::queue<LogEntry> m_queue;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
    std::thread m_worker;

    std::ofstream m_masterSink;
    std::ofstream m_hoverSink;
    std::ofstream m_analysisSink;
    std::ofstream m_symbolsSink;
    std::ofstream m_crashSink;
};
} // namespace angel_lsp::utils
