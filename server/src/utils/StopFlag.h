#pragma once

#include <atomic>

namespace angel_lsp::utils
{
/**
 * @brief A cancellation signal shared between a thread and whoever wants it to stop.
 *        This is `std::stop_token` reduced to the part this server uses, and it exists because
 */
class StopFlag
{
  public:
    StopFlag() = default;
    StopFlag(const StopFlag&) = delete;
    StopFlag& operator=(const StopFlag&) = delete;

    /**
     * @brief Asks the thread reading this flag to stop at its next check.
     */
    void Request() noexcept
    {
        m_stop.store(true, std::memory_order_relaxed);
    }

    /**
     * @brief Rearms the flag for a new thread. Only ever after the previous one was joined.
     */
    void Clear() noexcept
    {
        m_stop.store(false, std::memory_order_relaxed);
    }

    /**
     * @brief Named as `std::stop_token`'s member, so a checking site reads the same either way.
     */
    [[nodiscard]] bool stop_requested() const noexcept
    {
        return m_stop.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<bool> m_stop{false};
};
} // namespace angel_lsp::utils
