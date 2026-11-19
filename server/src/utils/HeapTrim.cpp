#include "utils/HeapTrim.h"

#include <cstdlib>

#if defined(_MSC_VER) || defined(__GLIBC__)
#include <malloc.h>
#endif

namespace angel_lsp::utils
{
void ReleaseFreeHeapMemory() noexcept
{
#if defined(_MSC_VER)
    _heapmin();
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}
void ConfigureServerHeap() noexcept
{
#if defined(__GLIBC__)
    mallopt(M_ARENA_MAX, 1);
#endif
}
} // namespace angel_lsp::utils
