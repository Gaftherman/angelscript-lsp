#pragma once

#include "features/formatting/FormattingTokens.h"
#include "features/formatting/FormattingTypes.h"
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Evaluates whether a whitespace separator is required between two adjacent tokens.
 * @param[in] tokens Annotated token stream.
 * @param[in] prevIdx Index of left-hand token.
 * @param[in] currIdx Index of right-hand token.
 * @param[in] options Effective formatting style options.
 * @return True if a space should be inserted between the tokens.
 */
bool NeedsSpaceBetween(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx,
                       const FormatCodeOptions& options);
} // namespace angel_lsp::features
