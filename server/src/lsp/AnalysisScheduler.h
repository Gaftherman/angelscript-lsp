#pragma once

#include "document/Document.h"
#include <ankerl/unordered_dense.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace angel_lsp
{
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
    PendingAnalysisEntry(std::string t, document::TreePtr tr, int v, uint64_t gen = 0, uint64_t cfgRev = 0)
        : text(std::move(t)), tree(std::move(tr)), version(v), generation(gen), configRevision(cfgRev)
    {
    }

    PendingAnalysisEntry(std::string t, TSTree* tr, int v, uint64_t gen = 0, uint64_t cfgRev = 0)
        : text(std::move(t)), tree(document::MakeTreePtr(tr)), version(v), generation(gen), configRevision(cfgRev)
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
    using AnalyzeCallback =
        std::function<void(const std::string& uriStr, const std::string& text, document::TreePtr tree, int version,
                           uint64_t generation, uint64_t configRevision)>;

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
     * @param uriStr Document URI key.
     * @param text Document analysis text.
     * @param force True to bypass deduplication when symbols moved without text changing.
     * @param tree Copied or newly parsed TSTree.
     * @param version Document version.
     * @param generation Document opening generation.
     * @param configRevision Revision number of relevant config/dependencies.
     */
    void Schedule(const std::string& uriStr, std::string text, bool force, document::TreePtr tree, int version,
                  uint64_t generation = 0, uint64_t configRevision = 0);

    /**
     * @brief Enqueues a document for analysis without debounce delay.
     *
     * Used when the workspace scan completes and open documents must be re-analysed
     * immediately: the 200 ms debounce serves no purpose because no further edits are
     * expected from the scan, and the user is already waiting.
     */
    void ScheduleImmediate(const std::string& uriStr, std::string text, bool force,
                           document::TreePtr tree = document::MakeTreePtr(nullptr), int version = -1,
                           uint64_t generation = 0, uint64_t configRevision = 0);

    /**
     * @brief Cancels pending analysis and marks document as saved on message loop.
     * @param uriStr Document URI key.
     * @param version Document saved version (-1 if unknown).
     */
    void MarkSaved(const std::string& uriStr, int version = -1);

    /**
     * @brief Cancels any pending analysis for a closed document.
     * @param uriStr Document URI key.
     */
    void Cancel(const std::string& uriStr);

    /**
     * @brief Queries whether current analysis for uri has been cancelled.
     * @param uriStr Document URI key.
     * @return True if cancelled.
     */
    [[nodiscard]] bool IsCancelled(const std::string& uriStr) const;

    /**
     * @brief Blocks the calling thread until all pending and currently executing
     *        background analysis tasks have finished.
     */
    void DrainQueue();

    /**
     * @brief Stops the background worker thread.
     */
    void Stop();

    /**
     * @brief Checks and updates peer analysis timestamp to debounce cascade passes.
     * @param uriStr Peer document URI.
     * @return True if cascade analysis should be suppressed due to active debounce window.
     */
    bool ShouldDebouncePeer(const std::string& uriStr);

    /**
     * @brief Clears peer debounce timestamp for a document.
     * @param uriStr Peer document URI.
     */
    void ClearPeerDebounce(const std::string& uriStr);

  private:
    void RunLoop();

    AnalyzeCallback m_callback;
    std::chrono::milliseconds m_debounceWindow;
    std::atomic<bool> m_stop{false};

    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_drainCv;
    std::atomic<size_t> m_activeTasks{0};
    uint64_t m_revision{0};

    ankerl::unordered_dense::map<std::string, PendingAnalysisEntry> m_pending;
    ankerl::unordered_dense::map<std::string, PendingAnalysisEntry> m_inFlight;
    std::atomic<bool> m_immediateRequested{false};
    std::string m_currentlyAnalyzingUri;
    std::string m_currentlyAnalyzingText;
    int m_currentlyAnalyzingVersion = -1;
    uint64_t m_currentlyAnalyzingGeneration = 0;
    std::atomic<bool> m_cancelCurrentAnalysis{false};
    ankerl::unordered_dense::set<std::string> m_savedUris;
    ankerl::unordered_dense::map<std::string, int> m_savedVersions;
    ankerl::unordered_dense::set<std::string> m_cancelledUris;

    mutable std::mutex m_peerMutex;
    ankerl::unordered_dense::map<std::string, std::chrono::steady_clock::time_point> m_peerTimestamps;
    static constexpr std::chrono::milliseconds k_peerDebounceWindow{250};
};
} // namespace angel_lsp
