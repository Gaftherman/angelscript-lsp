#pragma once

#include "document/Document.h"
#include <ankerl/unordered_dense.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace angel_lsp
{
/**
 * @brief Metadata for queued analysis requests.
 */
struct AnalysisMetadata
{
    int version = -1;
    uint64_t generation = 0;
    uint64_t configRevision = 0;
};

/**
 * @brief Request parameters for enqueuing a document in AnalysisScheduler.
 */
struct ScheduleRequest
{
    std::string uriStr;
    std::string text;
    bool force = false;
    document::TreePtr tree = document::MakeTreePtr(nullptr);
    int version = -1;
    uint64_t generation = 0;
    uint64_t configRevision = 0;
};

/**
 * @brief Request object passed to AnalyzeCallback when background worker executes.
 */
struct AnalyzeRequest
{
    std::string uriStr;
    std::string text;
    document::TreePtr tree = document::MakeTreePtr(nullptr);
    int version = -1;
    uint64_t generation = 0;
    uint64_t configRevision = 0;
};

/**
 * @brief Entry held in the analysis queue, owning a copied tree and document version.
 */
struct PendingAnalysisEntry
{
    std::string text;
    document::TreePtr tree = document::MakeTreePtr(nullptr);
    int version = -1;
    uint64_t generation = 0;
    uint64_t configRevision = 0;

    PendingAnalysisEntry() = default;
    PendingAnalysisEntry(std::string t, document::TreePtr tr, AnalysisMetadata meta)
        : text(std::move(t)), tree(std::move(tr)), version(meta.version), generation(meta.generation),
          configRevision(meta.configRevision)
    {
    }
    PendingAnalysisEntry(std::string t, document::TreePtr tr, int v = -1)
        : text(std::move(t)), tree(std::move(tr)), version(v), generation(0), configRevision(0)
    {
    }
    PendingAnalysisEntry(std::string t, TSTree* tr, int v = -1)
        : text(std::move(t)), tree(document::MakeTreePtr(tr)), version(v), generation(0), configRevision(0)
    {
    }

    PendingAnalysisEntry(const PendingAnalysisEntry&) = delete;
    PendingAnalysisEntry& operator=(const PendingAnalysisEntry&) = delete;
    PendingAnalysisEntry(PendingAnalysisEntry&&) noexcept = default;
    PendingAnalysisEntry& operator=(PendingAnalysisEntry&&) noexcept = default;

    /**
     * @brief Releases ownership of tree to raw pointer.
     */
    TSTree* ReleaseTree() noexcept
    {
        return tree.release();
    }
};

/**
 * @brief Debounced asynchronous scheduler for background semantic analysis and diagnostics.
 */
class AnalysisScheduler
{
  public:
    using AnalyzeCallback = std::function<void(AnalyzeRequest request)>;

    /**
     * @brief Constructs and launches the background analysis worker thread.
     * @param callback Function invoked per document to perform semantic analysis.
     * @param debounceWindow Debounce duration for keystroke bursts (default: 200 ms).
     */
    explicit AnalysisScheduler(AnalyzeCallback callback,
                               std::chrono::milliseconds debounceWindow = std::chrono::milliseconds(200));

    /**
     * @brief Stops worker thread and joins before destruction.
     */
    ~AnalysisScheduler();

    AnalysisScheduler(const AnalysisScheduler&) = delete;
    AnalysisScheduler& operator=(const AnalysisScheduler&) = delete;
    AnalysisScheduler(AnalysisScheduler&&) = delete;
    AnalysisScheduler& operator=(AnalysisScheduler&&) = delete;

    /**
     * @brief Enqueues a document for debounced analysis.
     * @param request Bundled schedule request parameters.
     */
    void Schedule(ScheduleRequest request);

    /**
     * @brief Enqueues a document for debounced analysis with defaults.
     * @param uriStr Document URI key.
     * @param text Document analysis text.
     * @param force True to bypass deduplication.
     * @param tree Copied or newly parsed TSTree.
     */
    void Schedule(const std::string& uriStr, std::string text, bool force, document::TreePtr tree);

    /**
     * @brief Enqueues a document for analysis without debounce delay.
     * @param request Bundled schedule request parameters.
     */
    void ScheduleImmediate(ScheduleRequest request);

    /**
     * @brief Enqueues a document for analysis without debounce delay with defaults.
     * @param uriStr Document URI key.
     * @param text Document analysis text.
     * @param force True to bypass deduplication.
     * @param tree Copied or newly parsed TSTree.
     */
    void ScheduleImmediate(const std::string& uriStr, std::string text, bool force, document::TreePtr tree);

    /**
     * @brief Cancels pending analysis and marks document as saved on message loop.
     * @param uriStr Document URI key.
     * @param version Document saved version (-1 if unknown).
     */
    void MarkSaved(const std::string& uriStr, int version = -1);

    /**
     * @brief Cancels any pending or active analysis for the given document URI.
     * @param uriStr Document URI key.
     */
    void Cancel(const std::string& uriStr);

    /**
     * @brief Checks if analysis was cancelled for a document URI.
     * @param uriStr Document URI key.
     * @return True if cancelled.
     */
    [[nodiscard]] bool IsCancelled(const std::string& uriStr) const;

    /**
     * @brief Checks and sets peer debounce status for a document.
     * @param uriStr Document URI key.
     * @return True if should debounce.
     */
    bool ShouldDebouncePeer(const std::string& uriStr);

    /**
     * @brief Clears peer debounce timestamp for a document.
     * @param uriStr Document URI key.
     */
    void ClearPeerDebounce(const std::string& uriStr);

    /**
     * @brief Flushes the queue and signals worker to stop.
     */
    void Stop();

    /**
     * @brief Blocks calling thread until queue is empty and all analysis is complete.
     */
    void DrainQueue();

  private:
    void RunLoop();
    bool IsRedundantWithActiveOrInFlightLocked(const ScheduleRequest& request) const;
    bool UpdatePendingOrDiscardLocked(ScheduleRequest& request);
    void WaitForDebounce(std::unique_lock<std::mutex>& lock);
    void DrainInFlightWork();
    std::optional<std::pair<std::string, PendingAnalysisEntry>> PopNextValidInFlightEntry();
    void FinishActiveAnalysis();
    void UpdateActiveTasksLocked();

    AnalyzeCallback m_callback;
    std::chrono::milliseconds m_debounceWindow;

    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_drainCv;

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_immediateRequested{false};
    std::atomic<bool> m_cancelCurrentAnalysis{false};
    std::atomic<size_t> m_activeTasks{0};

    uint64_t m_revision = 0;

    ankerl::unordered_dense::map<std::string, PendingAnalysisEntry> m_pending;
    ankerl::unordered_dense::map<std::string, PendingAnalysisEntry> m_inFlight;

    ankerl::unordered_dense::set<std::string> m_savedUris;
    ankerl::unordered_dense::map<std::string, int> m_savedVersions;
    ankerl::unordered_dense::set<std::string> m_cancelledUris;

    std::string m_currentlyAnalyzingUri;
    std::string m_currentlyAnalyzingText;
    int m_currentlyAnalyzingVersion = -1;
    uint64_t m_currentlyAnalyzingGeneration = 0;

    std::mutex m_peerMutex;
    ankerl::unordered_dense::map<std::string, std::chrono::steady_clock::time_point> m_peerTimestamps;
    static constexpr auto k_peerDebounceWindow = std::chrono::milliseconds(500);
};
} // namespace angel_lsp
