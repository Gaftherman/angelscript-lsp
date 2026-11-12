#include "lsp/pipeline/AnalysisScheduler.h"

#include "parser/SyntaxTree.h"

#include <chrono>
#include <mutex>
#include <utility>

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

template <typename Entry> static bool SameAnalysisContext(const Entry& entry, const ScheduleRequest& request)
{
    return entry.generation == request.generation && entry.configRevision == request.configRevision &&
           entry.closedRevision == request.closedRevision;
}

bool AnalysisScheduler::IsRedundantWithActiveOrInFlightLocked(const ScheduleRequest& request) const
{
    if (request.force || request.version < 0)
    {
        return false;
    }
    if (m_currentlyAnalyzingUri == request.uriStr && !m_cancelCurrentAnalysis.load() &&
        SameAnalysisContext(m_currentlyAnalyzing, request) && m_currentlyAnalyzing.version >= request.version &&
        m_currentlyAnalyzingText == request.text)
    {
        return true;
    }

    const auto running = m_inFlight.find(request.uriStr);
    return running != m_inFlight.end() && SameAnalysisContext(running->second, request) &&
           running->second.version >= request.version && running->second.text == request.text;
}

bool AnalysisScheduler::UpdatePendingOrDiscardLocked(ScheduleRequest& request)
{
    const auto it = m_pending.find(request.uriStr);
    if (it == m_pending.end())
    {
        m_pending.emplace(request.uriStr,
                          PendingAnalysisEntry{std::move(request.text), std::move(request.tree),
                                               AnalysisMetadata{request.version, request.generation,
                                                                request.configRevision, request.closedRevision},
                                               std::move(request.preprocessing)});
        return true;
    }

    if (!request.force && it->second.text == request.text && SameAnalysisContext(it->second, request))
    {
        if (request.version > it->second.version)
        {
            it->second.version = request.version;
            it->second.generation = request.generation;
            it->second.configRevision = request.configRevision;
            it->second.preprocessing = std::move(request.preprocessing);
        }
        return false;
    }

    if (!request.force && it->second.generation == request.generation && it->second.version > request.version &&
        request.version >= 0)
    {
        return false;
    }

    it->second = PendingAnalysisEntry{
        std::move(request.text), std::move(request.tree),
        AnalysisMetadata{request.version, request.generation, request.configRevision, request.closedRevision},
        std::move(request.preprocessing)};
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

void AnalysisScheduler::Schedule(const std::string& uriStr, std::string text, bool force, parser::TreePtr tree)
{
    Schedule(ScheduleRequest{uriStr, std::move(text), force, std::move(tree), -1, 0, 0});
}

void AnalysisScheduler::ScheduleImmediate(ScheduleRequest request)
{
    Schedule(std::move(request));
    m_immediateRequested.store(true);
    m_cv.notify_one();
}

void AnalysisScheduler::ScheduleImmediate(const std::string& uriStr, std::string text, bool force, parser::TreePtr tree)
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
    if (m_currentlyAnalyzingUri == uriStr && (version < 0 || m_currentlyAnalyzing.version <= version))
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
} // namespace angel_lsp
