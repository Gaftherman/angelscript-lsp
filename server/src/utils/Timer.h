#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <spdlog/fmt/fmt.h>
#include <string>

namespace angel_lsp::utils
{
/**
 * @brief Formats an elapsed duration in milliseconds into a human-readable string.
 *
 * Durations under 1000 ms are formatted as "{:.2f} ms" (e.g. "45.20 ms").
 * Durations of 1000 ms or greater are formatted with both seconds and milliseconds
 * (e.g. "25.00 s (25000.00 ms)").
 *
 * @param[in] ms Duration in milliseconds.
 * @return Formatted human-readable duration string.
 */
inline std::string FormatDuration(double ms)
{
    if (std::isnan(ms) || ms < 0.0)
    {
        ms = 0.0;
    }
    if (ms >= 1000.0)
    {
        return fmt::format("{:.2f} s ({:.2f} ms)", ms / 1000.0, ms);
    }
    return fmt::format("{:.2f} ms", ms);
}

/**
 * @brief High-resolution stopwatch using std::chrono::steady_clock.
 */
class HighResTimer
{
  private:
    std::chrono::steady_clock::time_point m_start;

  public:
    HighResTimer() noexcept : m_start(std::chrono::steady_clock::now())
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
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_start)
            .count();
    }

    /**
     * @brief Formats elapsed duration into human-readable string with units.
     * @return Formatted duration string.
     */
    [[nodiscard]] std::string FormatElapsed() const
    {
        return FormatDuration(ElapsedMs());
    }
};
} // namespace angel_lsp::utils
