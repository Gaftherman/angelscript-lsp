#include "lsp/pipeline/AnalysisScheduler.h"
#include "parser/SyntaxTree.h"
#include "utils/MultiFileLogger.h"

#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace angel_lsp
{
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
            itSaved != m_savedVersions.end() && currentEntry.version >= 0 && itSaved->second > currentEntry.version)
        {
            UpdateActiveTasksLocked();
            continue;
        }
        if (const auto itPending = m_pending.find(currentUri);
            itPending != m_pending.end() &&
            (currentEntry.version < 0 || itPending->second.version > currentEntry.version))
        {
            UpdateActiveTasksLocked();
            continue;
        }

        m_currentlyAnalyzingUri = currentUri;
        m_currentlyAnalyzingText = currentEntry.text;
        m_currentlyAnalyzing = {currentEntry.version, currentEntry.generation, currentEntry.configRevision,
                                currentEntry.closedRevision};
        m_cancelCurrentAnalysis = false;
        UpdateActiveTasksLocked();
        return std::make_pair(std::move(currentUri), std::move(currentEntry));
    }
    return std::nullopt;
}

void AnalysisScheduler::FinishActiveAnalysis()
{
    std::lock_guard<std::mutex> workLock(m_mutex);
    m_currentlyAnalyzingUri.clear();
    m_currentlyAnalyzingText.clear();
    m_currentlyAnalyzing = {};
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
                                          entry.generation, entry.configRevision, entry.closedRevision,
                                          std::move(entry.preprocessing)});
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
