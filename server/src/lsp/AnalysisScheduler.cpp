#include "lsp/AnalysisScheduler.h"

namespace angel_lsp
{
    AnalysisScheduler::AnalysisScheduler(AnalyzeCallback callback,
                                         std::chrono::milliseconds debounceWindow)
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
            m_cv.notify_all();
            if (m_thread.joinable())
            {
                m_thread.join();
            }
        }
    }

    void AnalysisScheduler::Schedule(const std::string &uriStr, std::string text, bool force,
                                     document::TreePtr tree, int version, uint64_t generation, uint64_t configRevision)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_savedUris.erase(uriStr);
            m_savedVersions.erase(uriStr);
            m_cancelledUris.erase(uriStr);

            if (!force && m_currentlyAnalyzingUri == uriStr &&
                m_currentlyAnalyzingVersion >= version && version >= 0 &&
                m_currentlyAnalyzingText == text)
            {
                return;
            }

            if (const auto running = m_inFlight.find(uriStr);
                !force && running != m_inFlight.end() && running->second.version >= version && version >= 0 && running->second.text == text)
            {
                return;
            }

            const auto it = m_pending.find(uriStr);
            if (it != m_pending.end())
            {
                if (!force && it->second.text == text)
                {
                    if (version > it->second.version)
                    {
                        it->second.version = version;
                        it->second.generation = generation;
                        it->second.configRevision = configRevision;
                    }
                    return;
                }

                if (!force && it->second.version > version && version >= 0)
                {
                    return;
                }

                it->second = PendingAnalysisEntry{ std::move(text), std::move(tree), version, generation, configRevision };
            }
            else
            {
                m_pending.emplace(uriStr, PendingAnalysisEntry{ std::move(text), std::move(tree), version, generation, configRevision });
            }

            ++m_revision;
        }

        m_cv.notify_one();
    }

    void AnalysisScheduler::MarkSaved(const std::string &uriStr, int version)
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
    }

    void AnalysisScheduler::Cancel(const std::string &uriStr)
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
        }
        ClearPeerDebounce(uriStr);
    }

    bool AnalysisScheduler::IsCancelled(const std::string &uriStr) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_currentlyAnalyzingUri == uriStr && m_cancelCurrentAnalysis.load())
        {
            return true;
        }
        return m_cancelledUris.contains(uriStr);
    }

    bool AnalysisScheduler::ShouldDebouncePeer(const std::string &uriStr)
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

    void AnalysisScheduler::ClearPeerDebounce(const std::string &uriStr)
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        m_peerTimestamps.erase(uriStr);
    }

    void AnalysisScheduler::RunLoop()
    {
        for (;;)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return m_stop.load() || !m_pending.empty(); });

            if (m_stop.load())
            {
                return;
            }

            for (;;)
            {
                const uint64_t seen = m_revision;
                const bool interrupted = m_cv.wait_for(lock, m_debounceWindow, [this, seen]()
                {
                    return m_stop.load() || m_revision != seen;
                });

                if (m_stop.load())
                {
                    return;
                }

                if (!interrupted)
                {
                    break;
                }
            }

            m_inFlight.swap(m_pending);
            lock.unlock();

            for (;;)
            {
                std::string currentUri;
                PendingAnalysisEntry currentEntry;
                {
                    std::lock_guard<std::mutex> workLock(m_mutex);
                    if (m_inFlight.empty())
                    {
                        break;
                    }
                    auto it = m_inFlight.begin();
                    currentUri = it->first;
                    currentEntry = std::move(it->second);
                    m_inFlight.erase(it);

                    if (m_cancelledUris.contains(currentUri) || m_savedUris.erase(currentUri) > 0)
                    {
                        continue;
                    }
                    if (const auto itSaved = m_savedVersions.find(currentUri);
                        itSaved != m_savedVersions.end() && itSaved->second >= currentEntry.version && currentEntry.version >= 0)
                    {
                        continue;
                    }
                    if (const auto itPending = m_pending.find(currentUri);
                        itPending != m_pending.end() && itPending->second.version > currentEntry.version && currentEntry.version >= 0)
                    {
                        // Superseded by newer edit while queued
                        continue;
                    }
                    m_currentlyAnalyzingUri = currentUri;
                    m_currentlyAnalyzingText = currentEntry.text;
                    m_currentlyAnalyzingVersion = currentEntry.version;
                    m_currentlyAnalyzingGeneration = currentEntry.generation;
                    m_cancelCurrentAnalysis = false;
                }

                if (m_callback && !m_cancelCurrentAnalysis.load())
                {
                    m_callback(currentUri, currentEntry.text, std::move(currentEntry.tree), currentEntry.version, currentEntry.generation, currentEntry.configRevision);
                }

                {
                    std::lock_guard<std::mutex> workLock(m_mutex);
                    m_currentlyAnalyzingUri.clear();
                    m_currentlyAnalyzingText.clear();
                    m_currentlyAnalyzingVersion = -1;
                    m_currentlyAnalyzingGeneration = 0;
                    m_cancelCurrentAnalysis = false;
                }
            }
        }
    }
}
