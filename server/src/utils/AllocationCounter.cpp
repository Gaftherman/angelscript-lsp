#include "utils/AllocationCounter.h"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace angel_lsp::utils
{
namespace
{
thread_local bool t_trackingActive = false;
thread_local size_t t_allocCount = 0;
thread_local size_t t_deallocCount = 0;
thread_local size_t t_allocBytes = 0;

std::atomic<ScopedAllocationCounter::TableCloneProvider> s_tableCloneProvider{nullptr};

#if defined(_MSC_VER) && defined(_DEBUG)
/**
 * @brief Windows CRT allocation hook callback capturing allocations and deallocations in debug builds.
 * @param[in] allocType Operation type (_HOOK_ALLOC, _HOOK_REALLOC, _HOOK_FREE).
 * @param[in] userData User data pointer.
 * @param[in] size Requested allocation size.
 * @param[in] blockType Memory block type.
 * @param[in] requestNumber Allocation sequence identifier.
 * @param[in] filename Source filename where allocation originated.
 * @param[in] lineNumber Source line number where allocation originated.
 * @return Non-zero integer to allow allocation to proceed.
 */
#define CRT_HOOK_ARGS                                                                                                  \
    int allocType, void *userData, size_t size, int blockType, long requestNumber, const unsigned char *filename,      \
        int lineNumber

int __cdecl CrtAllocationHook(CRT_HOOK_ARGS)
{
    (void)userData;
    (void)requestNumber;
    (void)filename;
    (void)lineNumber;

    if (blockType == _CRT_BLOCK)
    {
        return 1;
    }

    if (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC)
    {
        ScopedAllocationCounter::RecordAllocation(size);
    }
    else if (allocType == _HOOK_FREE)
    {
        ScopedAllocationCounter::RecordDeallocation();
    }
    return 1;
}

std::atomic<int> s_hookRefCount{0};
_CRT_ALLOC_HOOK s_previousHook = nullptr;
#endif

uint64_t QueryTableClones() noexcept
{
    auto provider = s_tableCloneProvider.load(std::memory_order_relaxed);
    return provider ? provider() : 0;
}
} // namespace

ScopedAllocationCounter::ScopedAllocationCounter(bool enable)
    : m_wasActive(t_trackingActive), m_startAllocations(t_allocCount), m_startDeallocations(t_deallocCount),
      m_startBytes(t_allocBytes), m_startTableClones(QueryTableClones())
{
#if defined(_MSC_VER) && defined(_DEBUG)
    if (s_hookRefCount.fetch_add(1, std::memory_order_seq_cst) == 0)
    {
        s_previousHook = _CrtSetAllocHook(CrtAllocationHook);
    }
#endif
    t_trackingActive = enable;
}

ScopedAllocationCounter::~ScopedAllocationCounter()
{
    t_trackingActive = m_wasActive;
#if defined(_MSC_VER) && defined(_DEBUG)
    if (s_hookRefCount.fetch_sub(1, std::memory_order_seq_cst) == 1)
    {
        _CrtSetAllocHook(s_previousHook);
    }
#endif
}

size_t ScopedAllocationCounter::GetAllocationCount() const noexcept
{
    return (t_allocCount >= m_startAllocations) ? (t_allocCount - m_startAllocations) : 0;
}

size_t ScopedAllocationCounter::GetDeallocationCount() const noexcept
{
    return (t_deallocCount >= m_startDeallocations) ? (t_deallocCount - m_startDeallocations) : 0;
}

size_t ScopedAllocationCounter::GetAllocatedBytes() const noexcept
{
    return (t_allocBytes >= m_startBytes) ? (t_allocBytes - m_startBytes) : 0;
}

size_t ScopedAllocationCounter::GetTableClones() const noexcept
{
    const uint64_t current = QueryTableClones();
    return (current >= m_startTableClones) ? static_cast<size_t>(current - m_startTableClones) : 0;
}

void ScopedAllocationCounter::Reset() noexcept
{
    m_startAllocations = t_allocCount;
    m_startDeallocations = t_deallocCount;
    m_startBytes = t_allocBytes;
    m_startTableClones = QueryTableClones();
}

bool ScopedAllocationCounter::IsActive() noexcept
{
    return t_trackingActive;
}

void ScopedAllocationCounter::RecordAllocation(size_t bytes) noexcept
{
    if (t_trackingActive)
    {
        ++t_allocCount;
        t_allocBytes += bytes;
    }
}

void ScopedAllocationCounter::RecordDeallocation() noexcept
{
    if (t_trackingActive)
    {
        ++t_deallocCount;
    }
}

void ScopedAllocationCounter::SetTableCloneProvider(TableCloneProvider provider) noexcept
{
    s_tableCloneProvider.store(provider, std::memory_order_relaxed);
}

} // namespace angel_lsp::utils

#if !defined(_DEBUG)
void* operator new(std::size_t size)
{
    angel_lsp::utils::ScopedAllocationCounter::RecordAllocation(size);
    void* ptr = std::malloc(size);
    if (!ptr)
    {
        throw std::bad_alloc();
    }
    return ptr;
}

void* operator new[](std::size_t size)
{
    angel_lsp::utils::ScopedAllocationCounter::RecordAllocation(size);
    void* ptr = std::malloc(size);
    if (!ptr)
    {
        throw std::bad_alloc();
    }
    return ptr;
}

void operator delete(void* ptr) noexcept
{
    if (ptr)
    {
        angel_lsp::utils::ScopedAllocationCounter::RecordDeallocation();
        std::free(ptr);
    }
}

void operator delete[](void* ptr) noexcept
{
    if (ptr)
    {
        angel_lsp::utils::ScopedAllocationCounter::RecordDeallocation();
        std::free(ptr);
    }
}

void operator delete(void* ptr, std::size_t size) noexcept
{
    (void)size;
    operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t size) noexcept
{
    (void)size;
    operator delete[](ptr);
}
#endif
