#include "lsp/DiagnosticsPipeline.h"

namespace angel_lsp
{

DiagnosticsPipeline::DiagnosticsPipeline() : m_scheduler(nullptr)
{
}

DiagnosticsPipeline::DiagnosticsPipeline(AnalysisScheduler* externalScheduler) : m_scheduler(externalScheduler)
{
}

DiagnosticsPipeline::DiagnosticsPipeline(AnalysisScheduler::AnalyzeCallback callback,
                                         std::chrono::milliseconds debounceWindow)
{
    if (callback)
    {
        m_ownedScheduler = std::make_unique<AnalysisScheduler>(std::move(callback), debounceWindow);
        m_scheduler = m_ownedScheduler.get();
    }
}

DiagnosticsPipeline::~DiagnosticsPipeline()
{
    StopScheduler();
}

void DiagnosticsPipeline::SetScheduler(AnalysisScheduler* externalScheduler) noexcept
{
    m_scheduler = externalScheduler;
}

void DiagnosticsPipeline::InitScheduler(AnalysisScheduler::AnalyzeCallback callback,
                                        std::chrono::milliseconds debounceWindow)
{
    StopScheduler();
    m_ownedScheduler = std::make_unique<AnalysisScheduler>(std::move(callback), debounceWindow);
    m_scheduler = m_ownedScheduler.get();
}

void DiagnosticsPipeline::Schedule(ScheduleRequest request)
{
    if (m_scheduler)
    {
        m_scheduler->Schedule(std::move(request));
    }
}

void DiagnosticsPipeline::ScheduleImmediate(ScheduleRequest request)
{
    if (m_scheduler)
    {
        m_scheduler->ScheduleImmediate(std::move(request));
    }
}

void DiagnosticsPipeline::MarkSaved(const std::string& uriStr, int version)
{
    if (m_scheduler)
    {
        m_scheduler->MarkSaved(uriStr, version);
    }
}

void DiagnosticsPipeline::Cancel(const std::string& uriStr)
{
    if (m_scheduler)
    {
        m_scheduler->Cancel(uriStr);
    }
}

bool DiagnosticsPipeline::IsCancelled(const std::string& uriStr) const
{
    if (m_scheduler)
    {
        return m_scheduler->IsCancelled(uriStr);
    }
    return false;
}

bool DiagnosticsPipeline::ShouldDebouncePeer(const std::string& uriStr)
{
    if (m_scheduler)
    {
        return m_scheduler->ShouldDebouncePeer(uriStr);
    }
    return false;
}

void DiagnosticsPipeline::ClearPeerDebounce(const std::string& uriStr)
{
    if (m_scheduler)
    {
        m_scheduler->ClearPeerDebounce(uriStr);
    }
}

void DiagnosticsPipeline::DrainQueue()
{
    if (m_scheduler)
    {
        m_scheduler->DrainQueue();
    }
}

void DiagnosticsPipeline::StopScheduler()
{
    if (m_ownedScheduler)
    {
        m_ownedScheduler->Stop();
    }
}

void DiagnosticsPipeline::CacheDiagnosticsSnapshot(const std::string& uri, DiagnosticsSnapshot snapshot)
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_diagnosticsCache[uri] = std::move(snapshot);
}

std::optional<DiagnosticsSnapshot> DiagnosticsPipeline::GetCachedSnapshot(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    const auto it = m_diagnosticsCache.find(uri);
    if (it != m_diagnosticsCache.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void DiagnosticsPipeline::InvalidateDiagnostics(const std::string& uri)
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_diagnosticsCache.erase(uri);
}

void DiagnosticsPipeline::ClearAllDiagnostics()
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_diagnosticsCache.clear();
}

uint64_t DiagnosticsPipeline::NextRevision() noexcept
{
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return ++m_diagnosticsRevision;
}

void DiagnosticsPipeline::SetDiagnosticSeverities(
    ankerl::unordered_dense::map<std::string, analysis::DiagnosticSeverity> severities)
{
    m_diagnosticSeverities = std::move(severities);
}

const ankerl::unordered_dense::map<std::string, analysis::DiagnosticSeverity>&
DiagnosticsPipeline::GetDiagnosticSeverities() const noexcept
{
    return m_diagnosticSeverities;
}

} // namespace angel_lsp
