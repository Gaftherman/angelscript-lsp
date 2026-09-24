#include "lsp/AnalysisScheduler.h"
#include "utils/MultiFileLogger.h"
#include <exception>

namespace angel_lsp
{
AnalysisScheduler::AnalysisScheduler(AnalyzeCallback callback, std::chrono::milliseconds debounceWindow)
    : m_callback(std::move(callback)), m_debounceWindow(debounceWindow)
{
    m_thread = std::thread(&AnalysisScheduler::RunLoop, this);
}

AnalysisScheduler::~AnalysisScheduler()
{
    Stop();
}

void AnalysisScheduler::Stop()
{
    if (!m_stop.exchange(true))
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.clear();
            m_inFlight.clear();
            m_cancelCurrentAnalysis = true;
            m_activeTasks.store(0);
        }
        m_cv.notify_all();
        m_drainCv.notify_all();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }
}

void AnalysisScheduler::DrainQueue()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_stop.load())
    {
        return;
    }

    if (!m_pending.empty())
    {
        m_immediateRequested.store(true);
        m_cv.notify_one();
    }

    m_drainCv.wait(
        lock, [this]()
        { return m_stop.load() || (m_pending.empty() && m_inFlight.empty() && m_currentlyAnalyzingUri.empty()); });
}

void AnalysisScheduler::UpdateActiveTasksLocked()
{
    const size_t remaining = m_pending.size() + m_inFlight.size() + (m_currentlyAnalyzingUri.empty() ? 0 : 1);
    m_activeTasks.store(remaining);
    if (remaining == 0)
    {
        m_drainCv.notify_all();
    }
}

bool AnalysisScheduler::IsRedundantWithActiveOrInFlightLocked(const ScheduleRequest& request) const
{
    if (request.force)
    {
        return false;
    }
    if (m_currentlyAnalyzingUri == request.uriStr && m_currentlyAnalyzingVersion >= request.version &&
        request.version >= 0 && m_currentlyAnalyzingText == request.text)
    {
        return true;
    }

    const auto running = m_inFlight.find(request.uriStr);
    return (running != m_inFlight.end() && running->second.version >= request.version && request.version >= 0 &&
            running->second.text == request.text);
}

bool AnalysisScheduler::UpdatePendingOrDiscardLocked(ScheduleRequest& request)
{
    const auto it = m_pending.find(request.uriStr);
    if (it == m_pending.end())
    {
        m_pending.emplace(request.uriStr, PendingAnalysisEntry{std::move(request.text), std::move(request.tree),
                                                               AnalysisMetadata{request.version, request.generation,
                                                                                request.configRevision}});
        return true;
    }

    if (!request.force && it->second.text == request.text)
    {
        if (request.version > it->second.version)
        {
            it->second.version = request.version;
            it->second.generation = request.generation;
            it->second.configRevision = request.configRevision;
        }
        return false;
    }

    if (!request.force && it->second.version > request.version && request.version >= 0)
    {
        return false;
    }

    it->second = PendingAnalysisEntry{std::move(request.text), std::move(request.tree),
                                      AnalysisMetadata{request.version, request.generation, request.configRevision}};
    return true;
}

void AnalysisScheduler::Schedule(ScheduleRequest request)
{
    if (m_stop.load())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_savedUris.erase(request.uriStr);
        m_savedVersions.erase(request.uriStr);
        m_cancelledUris.erase(request.uriStr);

        if (IsRedundantWithActiveOrInFlightLocked(request))
        {
            return;
        }

        if (!UpdatePendingOrDiscardLocked(request))
        {
            return;
        }

        ++m_revision;
        UpdateActiveTasksLocked();
    }

    m_cv.notify_one();
}

void AnalysisScheduler::Schedule(const std::string& uriStr, std::string text, bool force, document::TreePtr tree)
{
    Schedule(ScheduleRequest{uriStr, std::move(text), force, std::move(tree), -1, 0, 0});
}

void AnalysisScheduler::ScheduleImmediate(ScheduleRequest request)
{
    Schedule(std::move(request));
    m_immediateRequested.store(true);
    m_cv.notify_one();
}

void AnalysisScheduler::ScheduleImmediate(const std::string& uriStr, std::string text, bool force,
                                          document::TreePtr tree)
{
    ScheduleImmediate(ScheduleRequest{uriStr, std::move(text), force, std::move(tree), -1, 0, 0});
}

void AnalysisScheduler::MarkSaved(const std::string& uriStr, int version)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.erase(uriStr);
    m_inFlight.erase(uriStr);
    m_savedUris.insert(uriStr);
    if (version >= 0)
    {
        m_savedVersions[uriStr] = version;
    }
    if (m_currentlyAnalyzingUri == uriStr && (version < 0 || m_currentlyAnalyzingVersion <= version))
    {
        m_cancelCurrentAnalysis = true;
    }
    UpdateActiveTasksLocked();
}

void AnalysisScheduler::Cancel(const std::string& uriStr)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(uriStr);
        m_inFlight.erase(uriStr);
        m_savedUris.erase(uriStr);
        m_savedVersions.erase(uriStr);
        m_cancelledUris.insert(uriStr);
        if (m_currentlyAnalyzingUri == uriStr)
        {
            m_cancelCurrentAnalysis = true;
        }
        UpdateActiveTasksLocked();
    }
}

bool AnalysisScheduler::IsCancelled(const std::string& uriStr) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cancelledUris.contains(uriStr);
}

bool AnalysisScheduler::ShouldDebouncePeer(const std::string& uriStr)
{
    std::lock_guard<std::mutex> lock(m_peerMutex);
    const auto now = std::chrono::steady_clock::now();
    if (auto it = m_peerTimestamps.find(uriStr); it != m_peerTimestamps.end())
    {
        if (now - it->second < k_peerDebounceWindow)
        {
            return true;
        }
    }
    m_peerTimestamps[uriStr] = now;
    return false;
}

void AnalysisScheduler::ClearPeerDebounce(const std::string& uriStr)
{
    std::lock_guard<std::mutex> lock(m_peerMutex);
    m_peerTimestamps.erase(uriStr);
}

void AnalysisScheduler::WaitForDebounce(std::unique_lock<std::mutex>& lock)
{
    for (;;)
    {
        if (m_immediateRequested.load())
        {
            m_immediateRequested.store(false);
            break;
        }

        const uint64_t seen = m_revision;
        const bool interrupted =
            m_cv.wait_for(lock, m_debounceWindow, [this, seen]()
                          { return m_stop.load() || m_revision != seen || m_immediateRequested.load(); });

        if (m_stop.load())
        {
            return;
        }

        if (m_immediateRequested.load())
        {
            m_immediateRequested.store(false);
            break;
        }

        if (!interrupted)
        {
            break;
        }
    }
}

std::optional<std::pair<std::string, PendingAnalysisEntry>> AnalysisScheduler::PopNextValidInFlightEntry()
{
    std::lock_guard<std::mutex> workLock(m_mutex);
    while (!m_stop.load() && !m_inFlight.empty())
    {
        auto it = m_inFlight.begin();
        std::string currentUri = it->first;
        PendingAnalysisEntry currentEntry = std::move(it->second);
        m_inFlight.erase(it);

        if (m_cancelledUris.contains(currentUri) || m_savedUris.erase(currentUri) > 0)
        {
            UpdateActiveTasksLocked();
            continue;
        }
        if (const auto itSaved = m_savedVersions.find(currentUri);
            itSaved != m_savedVersions.end() && itSaved->second >= currentEntry.version && currentEntry.version >= 0)
        {
            UpdateActiveTasksLocked();
            continue;
        }
        if (const auto itPending = m_pending.find(currentUri); itPending != m_pending.end() &&
                                                               itPending->second.version > currentEntry.version &&
                                                               currentEntry.version >= 0)
        {
            UpdateActiveTasksLocked();
            continue;
        }

        m_currentlyAnalyzingUri = currentUri;
        m_currentlyAnalyzingText = currentEntry.text;
        m_currentlyAnalyzingVersion = currentEntry.version;
        m_currentlyAnalyzingGeneration = currentEntry.generation;
        m_cancelCurrentAnalysis = false;
        return std::make_pair(std::move(currentUri), std::move(currentEntry));
    }
    return std::nullopt;
}

void AnalysisScheduler::FinishActiveAnalysis()
{
    std::lock_guard<std::mutex> workLock(m_mutex);
    m_currentlyAnalyzingUri.clear();
    m_currentlyAnalyzingText.clear();
    m_currentlyAnalyzingVersion = -1;
    m_currentlyAnalyzingGeneration = 0;
    m_cancelCurrentAnalysis = false;
    UpdateActiveTasksLocked();
}

void AnalysisScheduler::DrainInFlightWork()
{
    for (;;)
    {
        if (m_stop.load())
        {
            break;
        }

        auto entryOpt = PopNextValidInFlightEntry();
        if (!entryOpt)
        {
            break;
        }

        auto& [uri, entry] = *entryOpt;
        if (m_callback && !m_cancelCurrentAnalysis.load())
        {
            try
            {
                m_callback(AnalyzeRequest{uri, std::move(entry.text), std::move(entry.tree), entry.version,
                                          entry.generation, entry.configRevision});
            }
            catch (const std::exception& e)
            {
                angel_lsp::utils::MultiFileLogger::Instance().LogCrash(
                    std::string("AnalysisScheduler worker caught exception: ") + e.what());
            }
            catch (...)
            {
                angel_lsp::utils::MultiFileLogger::Instance().LogCrash(
                    "AnalysisScheduler worker caught unknown fatal exception.");
            }
        }

        FinishActiveAnalysis();
    }
}

void AnalysisScheduler::RunLoop()
{
    while (!m_stop.load())
    {
        try
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return m_stop.load() || !m_pending.empty(); });

            if (m_stop.load())
            {
                return;
            }

            WaitForDebounce(lock);

            if (m_stop.load())
            {
                return;
            }

            m_inFlight.swap(m_pending);
            lock.unlock();

            DrainInFlightWork();
        }
        catch (const std::exception& e)
        {
            angel_lsp::utils::MultiFileLogger::Instance().LogCrash(
                std::string("AnalysisScheduler::RunLoop caught exception: ") + e.what());
        }
        catch (...)
        {
            angel_lsp::utils::MultiFileLogger::Instance().LogCrash(
                "AnalysisScheduler::RunLoop caught unknown fatal exception.");
        }
    }
}
} // namespace angel_lsp
