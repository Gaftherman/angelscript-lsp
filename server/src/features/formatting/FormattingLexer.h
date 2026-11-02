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

/**
 * @brief Cursor position, line counter, and buffer offsets for lexing source code formatting tokens.
 */
struct ScanState
{
    explicit ScanState(std::string_view source) : src(source)
    {
    }

    std::string_view src;
    size_t i = 0;
    uint32_t curLine = 0;
    uint32_t curCol = 0;
    uint32_t newlines = 0;
    std::vector<Token> tokens;
};
} // namespace angel_lsp::features
