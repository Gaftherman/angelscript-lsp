#pragma once

#include "features/formatting/FormattingTokens.h"
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Scans raw lexical tokens from AngelScript source without template or semantic adjustments.
 * @param[in] src Source text to scan.
 * @return Vector of raw scanned tokens.
 */
std::vector<Token> ScanRawTokens(std::string_view src);
} // namespace angel_lsp::features
