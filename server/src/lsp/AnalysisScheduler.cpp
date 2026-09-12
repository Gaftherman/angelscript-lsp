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
                                     document::TreePtr tree, int version)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_savedUris.erase(uriStr);

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
                    return;
                }

                if (!force && it->second.version > version && version >= 0)
                {
                    return;
                }

                it->second = PendingAnalysisEntry{ std::move(text), std::move(tree), version };
            }
            else
            {
                m_pending.emplace(uriStr, PendingAnalysisEntry{ std::move(text), std::move(tree), version });
            }

            ++m_revision;
        }

        m_cv.notify_one();
    }

    void AnalysisScheduler::MarkSaved(const std::string &uriStr)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(uriStr);
        m_savedUris.insert(uriStr);
    }

    void AnalysisScheduler::Cancel(const std::string &uriStr)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.erase(uriStr);
            m_savedUris.erase(uriStr);
        }
        ClearPeerDebounce(uriStr);
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

            for (auto &[uriStr, entry] : m_inFlight)
            {
                {
                    std::lock_guard<std::mutex> savedLock(m_mutex);
                    if (m_savedUris.erase(uriStr) > 0)
                    {
                        continue;
                    }
                    if (const auto it = m_pending.find(uriStr);
                        it != m_pending.end() && it->second.version > entry.version && entry.version >= 0)
                    {
                        // Superseded by newer edit while queued
                        continue;
                    }
                }

                if (m_callback)
                {
                    m_callback(uriStr, entry.text, std::move(entry.tree), entry.version);
                }
            }

            {
                std::lock_guard<std::mutex> doneLock(m_mutex);
                m_inFlight.clear();
            }
        }
    }
}
