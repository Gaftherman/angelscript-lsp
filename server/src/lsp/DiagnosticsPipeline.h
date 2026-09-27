#pragma once

#include "analysis/DiagnosticContext.h"
#include "lsp/AnalysisScheduler.h"

#include <ankerl/unordered_dense.h>
#include <chrono>
#include <functional>
#include <lsp/messages.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace angel_lsp
{

/**
 * @brief The last diagnostics computed for a document, cached post-conversion.
 */
struct DiagnosticsSnapshot
{
    /** @brief Monotonically increasing result ID string for LSP diagnostic pull protocol. */
    std::string resultId;

    /** @brief Converted LSP protocol diagnostics. */
    std::vector<lsp::Diagnostic> items;

    /** @brief Hash of the document text these diagnostics were computed from. */
    size_t textHash = 0;

    /** @brief Document version at the time analysis was performed. */
    int version = -1;

    /** @brief Document generation counter. */
    uint64_t generation = 0;

    /** @brief Configuration revision at the time analysis was performed. */
    uint64_t configRevision = 0;
};

/**
 * @brief Coordinates debounced background analysis scheduling and thread-safe diagnostic caching.
 */
class DiagnosticsPipeline
{
  public:
    /**
     * @brief Constructs DiagnosticsPipeline without an attached scheduler.
     */
    DiagnosticsPipeline();

    /**
     * @brief Constructs DiagnosticsPipeline wrapping an existing external scheduler.
     * @param[in] externalScheduler Pointer to externally managed AnalysisScheduler.
     */
    explicit DiagnosticsPipeline(AnalysisScheduler* externalScheduler);

    /**
     * @brief Constructs DiagnosticsPipeline owning a newly created AnalysisScheduler.
     * @param[in] callback Function invoked per document to perform semantic analysis.
     * @param[in] debounceWindow Debounce duration for keystroke bursts (default: 200 ms).
     */
    explicit DiagnosticsPipeline(AnalysisScheduler::AnalyzeCallback callback,
                                 std::chrono::milliseconds debounceWindow = std::chrono::milliseconds(200));

    /**
     * @brief Destructor. Ensures any owned background scheduler is stopped cleanly.
     */
    ~DiagnosticsPipeline();

    DiagnosticsPipeline(const DiagnosticsPipeline&) = delete;
    DiagnosticsPipeline& operator=(const DiagnosticsPipeline&) = delete;
    DiagnosticsPipeline(DiagnosticsPipeline&&) = delete;
    DiagnosticsPipeline& operator=(DiagnosticsPipeline&&) = delete;

    /**
     * @brief Attaches an external AnalysisScheduler.
     * @param[in] externalScheduler Pointer to external scheduler.
     */
    void SetScheduler(AnalysisScheduler* externalScheduler) noexcept;

    /**
     * @brief Initializes or rebinds an owned background analysis scheduler.
     * @param[in] callback Analysis callback.
     * @param[in] debounceWindow Debounce window.
     */
    void InitScheduler(AnalysisScheduler::AnalyzeCallback callback,
                       std::chrono::milliseconds debounceWindow = std::chrono::milliseconds(200));

    /**
     * @brief Enqueues a document for debounced analysis.
     * @param[in] request Bundled schedule request parameters.
     */
    void Schedule(ScheduleRequest request);

    /**
     * @brief Enqueues a document for analysis without debounce delay.
     * @param[in] request Bundled schedule request parameters.
     */
    void ScheduleImmediate(ScheduleRequest request);

    /**
     * @brief Cancels pending analysis and marks document as saved on message loop.
     * @param[in] uriStr Document URI key.
     * @param[in] version Document saved version (-1 if unknown).
     */
    void MarkSaved(const std::string& uriStr, int version = -1);

    /**
     * @brief Cancels any pending or active analysis for the given document URI.
     * @param[in] uriStr Document URI key.
     */
    void Cancel(const std::string& uriStr);

    /**
     * @brief Checks if analysis was cancelled for a document URI.
     * @param[in] uriStr Document URI key.
     * @return True if cancelled.
     */
    [[nodiscard]] bool IsCancelled(const std::string& uriStr) const;

    /**
     * @brief Checks and sets peer debounce status for a document.
     * @param[in] uriStr Document URI key.
     * @return True if should debounce.
     */
    bool ShouldDebouncePeer(const std::string& uriStr);

    /**
     * @brief Clears peer debounce timestamp for a document.
     * @param[in] uriStr Document URI key.
     */
    void ClearPeerDebounce(const std::string& uriStr);

    /**
     * @brief Waits until all scheduled and executing analysis tasks have completed.
     */
    void DrainQueue();

    /**
     * @brief Stops the background analysis scheduler and worker thread.
     */
    void StopScheduler();

    /**
     * @brief Stores a converted diagnostics snapshot into the thread-safe cache.
     * @param[in] uri Document canonical URI.
     * @param[in] snapshot Diagnostics snapshot to store.
     */
    void CacheDiagnosticsSnapshot(const std::string& uri, DiagnosticsSnapshot snapshot);

    /**
     * @brief Retrieves the cached diagnostics snapshot for a document.
     * @param[in] uri Document canonical URI.
     * @return Cached snapshot, or std::nullopt if not present.
     */
    [[nodiscard]] std::optional<DiagnosticsSnapshot> GetCachedSnapshot(const std::string& uri) const;

    /**
     * @brief Removes the cached diagnostics for a document (e.g. on document close).
     * @param[in] uri Document canonical URI.
     */
    void InvalidateDiagnostics(const std::string& uri);

    /**
     * @brief Clears all cached diagnostic snapshots.
     */
    void ClearAllDiagnostics();

    /**
     * @brief Generates next monotonic diagnostics cache revision.
     * @return Next revision integer.
     */
    [[nodiscard]] uint64_t NextRevision() noexcept;

    /**
     * @brief Sets configured diagnostic severity overrides.
     * @param[in] severities Map of diagnostic code to severity override.
     */
    void SetDiagnosticSeverities(ankerl::unordered_dense::map<std::string, analysis::DiagnosticSeverity> severities);

    /**
     * @brief Gets configured diagnostic severity overrides.
     * @return Const reference to severity overrides map.
     */
    [[nodiscard]] const ankerl::unordered_dense::map<std::string, analysis::DiagnosticSeverity>&
    GetDiagnosticSeverities() const noexcept;

  private:
    AnalysisScheduler* m_scheduler = nullptr;
    std::unique_ptr<AnalysisScheduler> m_ownedScheduler;
    mutable std::mutex m_cacheMutex;
    ankerl::unordered_dense::map<std::string, DiagnosticsSnapshot> m_diagnosticsCache;
    uint64_t m_diagnosticsRevision{0};
    ankerl::unordered_dense::map<std::string, analysis::DiagnosticSeverity> m_diagnosticSeverities;
};

} // namespace angel_lsp
