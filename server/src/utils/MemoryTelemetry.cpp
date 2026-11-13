#include "utils/MemoryTelemetry.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#pragma comment(lib, "psapi.lib")
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <fstream>
#include <unistd.h>
#endif

namespace angel_lsp::utils
{

void GetProcessMemoryUsage(size_t& workingSetBytes, size_t& privateUsageBytes) noexcept
{
    workingSetBytes = 0;
    privateUsageBytes = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
    {
        workingSetBytes = static_cast<size_t>(pmc.WorkingSetSize);
        privateUsageBytes = static_cast<size_t>(pmc.PrivateUsage);
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
    {
        workingSetBytes = static_cast<size_t>(info.resident_size);
        privateUsageBytes = static_cast<size_t>(info.virtual_size);
    }
#else
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open())
    {
        unsigned long sizePages = 0;
        unsigned long residentPages = 0;
        if (statm >> sizePages >> residentPages)
        {
            const long pageSize = sysconf(_SC_PAGESIZE);
            if (pageSize > 0)
            {
                workingSetBytes = static_cast<size_t>(residentPages) * static_cast<size_t>(pageSize);
                privateUsageBytes = static_cast<size_t>(sizePages) * static_cast<size_t>(pageSize);
            }
        }
    }
#endif
}

MemoryTelemetryReport SampleMemoryTelemetry(size_t symbolTableBytes, size_t scopeTreeBytes, size_t nodeIndexBytes,
                                            size_t docStoreBytes, double lastLatencyMs) noexcept
{
    MemoryTelemetryReport report;
    GetProcessMemoryUsage(report.workingSetBytes, report.privateUsageBytes);
    report.symbolTableBytes = symbolTableBytes;
    report.scopeTreeBytes = scopeTreeBytes;
    report.nodeIndexBytes = nodeIndexBytes;
    report.documentStoreBytes = docStoreBytes;
    report.lastAnalysisLatencyMs = lastLatencyMs;
    return report;
}

} // namespace angel_lsp::utils
