#pragma once

#include <atomic>

namespace angel_lsp::utils
{
    /**
     * @brief A cancellation signal shared between a thread and whoever wants it to stop.
     *
     * This is `std::stop_token` reduced to the part this server uses, and it exists because
     * `std::jthread` and `std::stop_token` are not portable yet: libc++ shipped them behind an
     * experimental flag, so the whole of Server.cpp failed to compile on macOS with
     *
     *     error: no type named 'jthread' in namespace 'std'
     *
     * A flag rather than a conditional `#if __cpp_lib_jthread`: two implementations of the same
     * lifecycle, one of which never runs on the machine the developer tests on, is how a
     * platform-specific deadlock gets written. This one behaves identically everywhere.
     *
     * What is deliberately *not* here is `std::stop_callback`. Its only use was to wake a
     * condition variable when a stop was requested, and the code that requests the stop already
     * holds that condition variable - so it notifies directly and the registration machinery, with
     * its lifetime and re-entrancy rules, buys nothing.
     *
     * Not copyable, and read from another thread by design: every access is atomic. `relaxed` is
     * enough, because the flag carries no data - the reader only has to observe the change
     * eventually, and every place it is checked is inside a loop that will check again.
     */
    class StopFlag
    {
    public:
        StopFlag() = default;
        StopFlag(const StopFlag &) = delete;
        StopFlag &operator=(const StopFlag &) = delete;

        /** @brief Asks the thread reading this flag to stop at its next check. */
        void Request() noexcept { m_stop.store(true, std::memory_order_relaxed); }

        /** @brief Rearms the flag for a new thread. Only ever after the previous one was joined. */
        void Clear() noexcept { m_stop.store(false, std::memory_order_relaxed); }

        /** @brief Named as `std::stop_token`'s member, so a checking site reads the same either way. */
        [[nodiscard]] bool stop_requested() const noexcept { return m_stop.load(std::memory_order_relaxed); }

    private:
        std::atomic<bool> m_stop{ false };
    };
}
