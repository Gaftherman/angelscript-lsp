#pragma once

#include <string_view>

namespace angel_lsp::utils
{
/**
 * @brief Matches a whole candidate against a user-configured regular expression, compiling each pattern once.
 *        Compiled expressions are cached per thread and keyed by pattern text, so repeated rule evaluation
 *        over many symbols never recompiles. A pattern that fails to compile is cached as unusable.
 * @param[in] candidate Text that must match the pattern in full.
 * @param[in] pattern ECMAScript regular expression source.
 * @return True when both arguments are non-empty, the pattern compiles and candidate matches it entirely.
 */
[[nodiscard]] bool MatchesCachedRegex(std::string_view candidate, std::string_view pattern);
} // namespace angel_lsp::utils
