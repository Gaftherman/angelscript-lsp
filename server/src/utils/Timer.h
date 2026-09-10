#pragma once

#include <chrono>
#include <cstdint>

namespace angel_lsp::utils
{
    /**
     * @brief High-resolution stopwatch using std::chrono::steady_clock.
     */
    class HighResTimer
    {
    private:
        std::chrono::steady_clock::time_point m_start;

    public:
        HighResTimer() noexcept
            : m_start(std::chrono::steady_clock::now())
        {
        }

        /**
         * @brief Resets the starting time point to now.
         */
        void Reset() noexcept
        {
            m_start = std::chrono::steady_clock::now();
        }

        /**
         * @brief Calculates elapsed duration in milliseconds.
         * @return Elapsed milliseconds as double.
         */
        [[nodiscard]] double ElapsedMs() const noexcept
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
        }

        /**
         * @brief Calculates elapsed duration in microseconds.
         * @return Elapsed microseconds as int64_t.
         */
        [[nodiscard]] int64_t ElapsedUs() const noexcept
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_start).count();
        }
    };
}
