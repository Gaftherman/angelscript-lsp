#pragma once

#include <cstddef>
#include <cstdint>

namespace angel_lsp::utils
{
/**
 * @brief RAII scoped allocation counter that measures heap allocations, deallocations, and table clones.
 *
 * Employs Windows CRT allocation hooks (_CrtSetAllocHook) when compiled under MSVC Debug,
 * and thread-local tracking with global operator new/delete in release configurations.
 */
class ScopedAllocationCounter
{
  public:
    using TableCloneProvider = uint64_t (*)();

    /**
     * @brief Constructs an allocation counter and begins tracking on the current thread.
     * @param[in] enable Whether to activate tracking immediately (defaults to true).
     */
    explicit ScopedAllocationCounter(bool enable = true);

    /**
     * @brief Destructs the counter, restoring previous hook and tracking state.
     */
    ~ScopedAllocationCounter();

    ScopedAllocationCounter(const ScopedAllocationCounter&) = delete;
    ScopedAllocationCounter& operator=(const ScopedAllocationCounter&) = delete;
    ScopedAllocationCounter(ScopedAllocationCounter&&) = delete;
    ScopedAllocationCounter& operator=(ScopedAllocationCounter&&) = delete;

    /**
     * @brief Returns the number of heap allocations observed during this counter's lifetime.
     * @return Count of heap allocations.
     */
    [[nodiscard]] size_t GetAllocationCount() const noexcept;

    /**
     * @brief Returns the number of heap deallocations observed during this counter's lifetime.
     * @return Count of heap deallocations.
     */
    [[nodiscard]] size_t GetDeallocationCount() const noexcept;

    /**
     * @brief Returns the total bytes allocated on the heap during this counter's lifetime.
     * @return Total allocated bytes.
     */
    [[nodiscard]] size_t GetAllocatedBytes() const noexcept;

    /**
     * @brief Returns the number of table clones created during this counter's lifetime.
     * @return Count of table clones.
     */
    [[nodiscard]] size_t GetTableClones() const noexcept;

    /**
     * @brief Resets the baseline starting metrics to current values.
     */
    void Reset() noexcept;

    /**
     * @brief Checks if allocation tracking is currently active on the calling thread.
     * @return True if tracking is active.
     */
    [[nodiscard]] static bool IsActive() noexcept;

    /**
     * @brief Records a heap allocation of the specified size.
     * @param[in] bytes Allocation size in bytes.
     */
    static void RecordAllocation(size_t bytes) noexcept;

    /**
     * @brief Records a heap deallocation.
     */
    static void RecordDeallocation() noexcept;

    /**
     * @brief Registers an external provider function for tracking table clone operations.
     * @param[in] provider Function pointer returning the cumulative table clone count.
     */
    static void SetTableCloneProvider(TableCloneProvider provider) noexcept;

  private:
    bool m_wasActive = false;
    size_t m_startAllocations = 0;
    size_t m_startDeallocations = 0;
    size_t m_startBytes = 0;
    uint64_t m_startTableClones = 0;
};
} // namespace angel_lsp::utils
