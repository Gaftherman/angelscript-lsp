#pragma once

#include <cstddef>

namespace angel_lsp::test
{

/**
 * @brief Reads the current resident set size of this process.
 * @return Resident bytes, or 0 when the platform query is unavailable.
 */
size_t CurrentResidentBytes();

/**
 * @brief Reads the highest resident set size reached since the last ResetPeakMemory call.
 * @return Peak resident bytes, or 0 when the platform query is unavailable.
 * @note On platforms that cannot reset the peak this is the lifetime peak of the process.
 */
size_t PeakResidentBytes();

/**
 * @brief Restarts peak tracking so PeakResidentBytes reflects only what runs afterwards.
 * @return True when the platform supports resetting the peak, false when the lifetime peak is kept.
 */
bool ResetPeakMemory();

} // namespace angel_lsp::test
