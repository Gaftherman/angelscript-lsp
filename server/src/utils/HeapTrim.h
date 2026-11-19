#pragma once

namespace angel_lsp::utils
{
/**
 * @brief Returns freed heap pages to the operating system.
 * @note Uses `malloc_trim` on glibc and `_heapmin` on MSVC; a no-op elsewhere. Never frees live allocations.
 */
void ReleaseFreeHeapMemory() noexcept;

/**
 * @brief Limits glibc to one malloc arena so analysis threads reuse freed memory.
 * @note Call once at process start, before any thread is created; a no-op outside glibc.
 */
void ConfigureServerHeap() noexcept;
} // namespace angel_lsp::utils
