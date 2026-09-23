#include "utils/MultiFileLogger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <system_error>
#include <vector>

#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#endif

namespace angel_lsp::utils
{
namespace
{
std::string GetCurrentDateString(std::chrono::system_clock::time_point time)
{
    const std::time_t tt = std::chrono::system_clock::to_time_t(time);
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &tt);
#else
    localtime_r(&tt, &tmBuf);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday);
    return std::string(buf);
}

std::string_view ChannelToString(LogChannel channel)
{
    switch (channel)
    {
    case LogChannel::Master:
        return "MASTER";
    case LogChannel::Hover:
        return "HOVER";
    case LogChannel::Analysis:
        return "ANALYSIS";
    case LogChannel::Symbols:
        return "SYMBOLS";
    case LogChannel::Crash:
        return "CRASH";
    }
    return "UNKNOWN";
}

std::string_view LevelToString(MultiFileLogLevel level)
{
    switch (level)
    {
    case MultiFileLogLevel::Debug:
        return "DEBUG";
    case MultiFileLogLevel::Info:
        return "INFO";
    case MultiFileLogLevel::Warn:
        return "WARN";
    case MultiFileLogLevel::Error:
        return "ERROR";
    case MultiFileLogLevel::Fatal:
        return "FATAL";
    }
    return "UNKNOWN";
}
} // namespace

MultiFileLogger::MultiFileLogger()
    : m_running(true)
{
    m_worker = std::thread(&MultiFileLogger::WorkerLoop, this);
}

MultiFileLogger::MultiFileLogger(const std::filesystem::path& logDirectory)
    : m_running(true)
{
    m_worker = std::thread(&MultiFileLogger::WorkerLoop, this);
    Initialize(logDirectory);
}

MultiFileLogger::~MultiFileLogger()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
    }
    m_cv.notify_all();
    if (m_worker.joinable())
    {
        m_worker.join();
    }
    std::lock_guard<std::mutex> sinkLock(m_sinkMutex);
    CloseSinks();
}

MultiFileLogger& MultiFileLogger::Instance()
{
    static MultiFileLogger instance;
    return instance;
}

std::string MultiFileLogger::FormatTimestamp(std::chrono::system_clock::time_point time)
{
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(time.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(time);
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &tt);
#else
    localtime_r(&tt, &tmBuf);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec,
                  static_cast<long long>(ms.count()));
    return std::string(buf);
}

std::string MultiFileLogger::FormatLogLine(const LogEntry& entry)
{
    std::string line;
    line.reserve(entry.message.size() + 80);
    line += "[";
    line += FormatTimestamp(entry.timestamp);
    line += "] [";
    line += ChannelToString(entry.channel);
    line += "] [TID:";
    line += std::to_string(entry.threadId);
    line += "] [";
    line += LevelToString(entry.level);
    line += "] ";
    line += entry.message;
    if (entry.durationMs >= 0.0)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " (%.2fms)", entry.durationMs);
        line += buf;
    }
    line += "\n";
    return line;
}

void MultiFileLogger::Initialize(const std::filesystem::path& baseDirectory)
{
    const std::string dateStr = GetCurrentDateString(std::chrono::system_clock::now());
    std::filesystem::path targetDir = baseDirectory;

    const std::string filenameStr = baseDirectory.filename().string();
    if (filenameStr.rfind("logs-", 0) == 0)
    {
        targetDir = baseDirectory;
    }
    else if (filenameStr == "lsp")
    {
        targetDir = baseDirectory / ("logs-" + dateStr);
    }
    else if (filenameStr == ".vscode")
    {
        targetDir = baseDirectory / "lsp" / ("logs-" + dateStr);
    }
    else if (baseDirectory.string().find(".vscode") != std::string::npos)
    {
        targetDir = baseDirectory / ("logs-" + dateStr);
    }
    else
    {
        targetDir = baseDirectory / ".vscode" / "lsp" / ("logs-" + dateStr);
    }

    std::error_code ec;
    std::filesystem::create_directories(targetDir, ec);

    std::lock_guard<std::mutex> sinkLock(m_sinkMutex);
    CloseSinks();
    m_logDirectory = targetDir;
    EnsureSinksOpen();
    m_initialized = (m_masterSink.is_open());
}

void MultiFileLogger::Log(LogChannel channel, MultiFileLogLevel level, std::string_view message, double durationMs)
{
    LogEntry entry;
    entry.channel = channel;
    entry.level = level;
    entry.timestamp = std::chrono::system_clock::now();
#if defined(_WIN32)
    entry.threadId = static_cast<uint64_t>(GetCurrentThreadId());
#else
    entry.threadId = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    entry.message = std::string(message);
    entry.durationMs = durationMs;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(entry));
    }
    m_cv.notify_one();
}

void MultiFileLogger::LogMaster(MultiFileLogLevel level, std::string_view message)
{
    Log(LogChannel::Master, level, message);
}

void MultiFileLogger::LogHover(MultiFileLogLevel level, std::string_view message, double durationMs)
{
    Log(LogChannel::Hover, level, message, durationMs);
}

void MultiFileLogger::LogAnalysis(MultiFileLogLevel level, std::string_view message, double durationMs)
{
    Log(LogChannel::Analysis, level, message, durationMs);
}

void MultiFileLogger::LogSymbols(MultiFileLogLevel level, std::string_view message, double durationMs)
{
    Log(LogChannel::Symbols, level, message, durationMs);
}

void MultiFileLogger::LogCrash(std::string_view message)
{
    Log(LogChannel::Crash, MultiFileLogLevel::Fatal, message);
    Flush();
}

void MultiFileLogger::Flush()
{
    std::vector<LogEntry> batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty())
        {
            batch.push_back(std::move(m_queue.front()));
            m_queue.pop();
        }
    }
    std::lock_guard<std::mutex> sinkLock(m_sinkMutex);
    for (const auto& entry : batch)
    {
        WriteEntry(entry);
    }
    FlushAllSinks();
}

std::filesystem::path MultiFileLogger::GetActiveLogDirectory() const
{
    std::lock_guard<std::mutex> lock(m_sinkMutex);
    return m_logDirectory;
}

bool MultiFileLogger::IsInitialized() const
{
    return m_initialized.load();
}

void MultiFileLogger::WorkerLoop()
{
    while (m_running)
    {
        std::vector<LogEntry> batch;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                return !m_running || !m_queue.empty();
            });

            while (!m_queue.empty())
            {
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop();
            }
        }

        if (!batch.empty())
        {
            ProcessBatch(batch);
        }
        else
        {
            std::lock_guard<std::mutex> sinkLock(m_sinkMutex);
            FlushAllSinks();
        }
    }

    std::vector<LogEntry> remaining;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty())
        {
            remaining.push_back(std::move(m_queue.front()));
            m_queue.pop();
        }
    }
    if (!remaining.empty())
    {
        ProcessBatch(remaining);
    }
    std::lock_guard<std::mutex> sinkLock(m_sinkMutex);
    FlushAllSinks();
}

void MultiFileLogger::ProcessBatch(std::vector<LogEntry>& batch)
{
    std::lock_guard<std::mutex> lock(m_sinkMutex);
    for (const auto& entry : batch)
    {
        WriteEntry(entry);
    }
}

std::ofstream* MultiFileLogger::GetChannelSink(LogChannel channel)
{
    switch (channel)
    {
    case LogChannel::Hover:
        return &m_hoverSink;
    case LogChannel::Analysis:
        return &m_analysisSink;
    case LogChannel::Symbols:
        return &m_symbolsSink;
    case LogChannel::Crash:
        return &m_crashSink;
    case LogChannel::Master:
    default:
        return nullptr;
    }
}

void MultiFileLogger::WriteEntry(const LogEntry& entry)
{
    if (!m_initialized)
    {
        return;
    }

    const std::string line = FormatLogLine(entry);
    if (m_masterSink.is_open())
    {
        m_masterSink << line;
    }

    std::ofstream* channelSink = GetChannelSink(entry.channel);
    if (channelSink && channelSink->is_open())
    {
        *channelSink << line;
    }

    const bool shouldFlush = (entry.level >= MultiFileLogLevel::Warn ||
                              entry.channel == LogChannel::Crash);
    if (shouldFlush)
    {
        if (m_masterSink.is_open())
        {
            m_masterSink.flush();
        }
        if (channelSink && channelSink->is_open())
        {
            channelSink->flush();
        }
    }
}

void MultiFileLogger::FlushAllSinks()
{
    if (m_masterSink.is_open()) m_masterSink.flush();
    if (m_hoverSink.is_open()) m_hoverSink.flush();
    if (m_analysisSink.is_open()) m_analysisSink.flush();
    if (m_symbolsSink.is_open()) m_symbolsSink.flush();
    if (m_crashSink.is_open()) m_crashSink.flush();
}

void MultiFileLogger::EnsureSinksOpen()
{
    if (m_logDirectory.empty())
    {
        return;
    }
    m_masterSink.open(m_logDirectory / "master.log", std::ios::out | std::ios::app);
    m_hoverSink.open(m_logDirectory / "hover.log", std::ios::out | std::ios::app);
    m_analysisSink.open(m_logDirectory / "analysis.log", std::ios::out | std::ios::app);
    m_symbolsSink.open(m_logDirectory / "symbols.log", std::ios::out | std::ios::app);
    m_crashSink.open(m_logDirectory / "crash.log", std::ios::out | std::ios::app);
}

void MultiFileLogger::CloseSinks()
{
    FlushAllSinks();
    if (m_masterSink.is_open()) m_masterSink.close();
    if (m_hoverSink.is_open()) m_hoverSink.close();
    if (m_analysisSink.is_open()) m_analysisSink.close();
    if (m_symbolsSink.is_open()) m_symbolsSink.close();
    if (m_crashSink.is_open()) m_crashSink.close();
}
} // namespace angel_lsp::utils
