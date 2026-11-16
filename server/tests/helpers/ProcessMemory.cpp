#include "helpers/ProcessMemory.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#elif defined(__linux__)
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#endif

namespace angel_lsp::test
{

#if defined(__linux__)
/**
 * @brief Reads one `Name:   <n> kB` field from /proc/self/status.
 * @param[in] field Field name including the trailing colon, for example "VmRSS:".
 * @return Value in bytes, or 0 when the field is missing.
 */
static size_t ReadStatusKilobytes(const char* field)
{
    std::ifstream status("/proc/self/status");
    std::string line;
    const size_t length = std::strlen(field);
    while (std::getline(status, line))
    {
        if (line.compare(0, length, field) == 0)
        {
            return static_cast<size_t>(std::strtoull(line.c_str() + length, nullptr, 10)) * 1024;
        }
    }
    return 0;
}
#endif

size_t CurrentResidentBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.WorkingSetSize : 0;
#elif defined(__linux__)
    return ReadStatusKilobytes("VmRSS:");
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
                   KERN_SUCCESS
               ? info.resident_size
               : 0;
#else
    return 0;
#endif
}

size_t PeakResidentBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.PeakWorkingSetSize : 0;
#elif defined(__linux__)
    return ReadStatusKilobytes("VmHWM:");
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
                   KERN_SUCCESS
               ? info.resident_size_max
               : 0;
#else
    return 0;
#endif
}

bool ResetPeakMemory()
{
#if defined(__linux__)
    std::ofstream clearRefs("/proc/self/clear_refs");
    if (!clearRefs.is_open())
    {
        return false;
    }
    clearRefs << "5";
    clearRefs.flush();
    return clearRefs.good();
#else
    return false;
#endif
}

} // namespace angel_lsp::test
