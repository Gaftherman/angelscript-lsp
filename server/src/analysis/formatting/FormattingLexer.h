#pragma once

#include "analysis/formatting/FormattingTokens.h"

#include <string_view>
#include <vector>

namespace angel_lsp::analysis::formatting
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

/**
 * @brief Attempts to consume a single-line comment sequence ('//') from the lexer stream.
 * @param[in,out] state Active lexer scan state.
 * @return True if a line comment was recognized and consumed; otherwise false.
 */
bool TryConsumeLineComment(ScanState& state);

/**
 * @brief Try consume block comment.
 * @param[in,out] state Parser or lexer scan state.
 * @return True if the condition is satisfied; false otherwise.
 */
bool TryConsumeBlockComment(ScanState& state);

bool TryConsumePreprocessor(ScanState& state);

/**
 * @brief Try consume raw string.
 * @param[in,out] state Parser or lexer scan state.
 * @return True if the condition is satisfied; false otherwise.
 */
bool TryConsumeRawString(ScanState& state);

bool TryConsumeQuotedLiteral(ScanState& state, char quoteChar, TokenType tokenType);

} // namespace angel_lsp::analysis::formatting
