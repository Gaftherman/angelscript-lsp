#pragma once

#include <cstddef>

namespace angel_lsp::utils
{

/**
 * @brief Memory usage and performance metrics for the language server process and data structures.
 */
struct MemoryTelemetryReport
{
    size_t workingSetBytes = 0;
    size_t privateUsageBytes = 0;
    size_t symbolTableBytes = 0;
    size_t scopeTreeBytes = 0;
    size_t nodeIndexBytes = 0;
    size_t documentStoreBytes = 0;
    double lastAnalysisLatencyMs = 0.0;
};

/**
 * @brief Retrieves the current OS process memory counters.
 * @param[out] workingSetBytes Process resident set size (WorkingSetSize) in bytes.
 * @param[out] privateUsageBytes Process private allocated memory in bytes.
 */
void GetProcessMemoryUsage(size_t& workingSetBytes, size_t& privateUsageBytes) noexcept;

/**
 * @brief Creates a memory and latency report combining OS metrics and internal data structure sizes.
 * @param[in] symbolTableBytes Estimated SymbolTable bytes.
 * @param[in] scopeTreeBytes Estimated ScopeTree bytes.
 * @param[in] nodeIndexBytes Estimated NodeIndex bytes.
 * @param[in] docStoreBytes Estimated DocumentStore bytes.
 * @param[in] lastLatencyMs Last analysis latency in milliseconds.
 * @return Populated MemoryTelemetryReport.
 */
MemoryTelemetryReport SampleMemoryTelemetry(size_t symbolTableBytes = 0, size_t scopeTreeBytes = 0,
                                            size_t nodeIndexBytes = 0, size_t docStoreBytes = 0,
                                            double lastLatencyMs = 0.0) noexcept;

} // namespace angel_lsp::utils
