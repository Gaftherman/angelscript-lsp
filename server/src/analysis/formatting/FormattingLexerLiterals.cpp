#include "analysis/formatting/FormattingLexer.h"

#include <cctype>
#include <string_view>

namespace angel_lsp::analysis::formatting
{

/**
 * @brief Scans and emits a single-line comment beginning with `//` up to the terminating newline or end of input during
 * source code formatting tokenization.
 * @param[in,out] state Active lexer scanning state whose cursor index must point at the candidate comment delimiter; on
 * success, advances past the comment content.
 * @return True if a double-slash line comment was recognized and consumed; false if the delimiter is absent, leaving
 * the scan state unmodified.
 * @note On success, advances the state cursor and column, resets state.newlines to zero, and appends a
 * TokenType::LineComment token whose text view borrows from state.src.
 */
bool TryConsumeLineComment(ScanState& state)
{
    char c = state.src[state.i];
    if (c == '/' && state.i + 1 < state.src.size() && state.src[state.i + 1] == '/')
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        while (state.i < state.src.size() && state.src[state.i] != '\r' && state.src[state.i] != '\n')
        {
            state.i++;
            state.curCol++;
        }
        state.tokens.push_back({TokenType::LineComment, state.src.substr(start, state.i - start), state.curLine,
                                startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

/**
 * @brief Scans and emits a C-style block comment starting with the slash-star sequence through its closing delimiter or
 * the end of input, tracking line counts and column offsets for layout formatting.
 * @param[in,out] state Active lexer scanning state whose cursor index must point at the opening slash-star delimiter;
 * on success, advances past the closing delimiter or to the end of input.
 * @return True if an opening slash-star delimiter was found and the comment consumed; false if the opening delimiter is
 * absent, leaving the scan state unmodified.
 * @note On success, advances the state cursor, updates line and column coordinates, resets state.newlines to zero, and
 * appends a TokenType::BlockComment token referencing state.src.
 */
bool TryConsumeBlockComment(ScanState& state)
{
    char c = state.src[state.i];
    if (c == '/' && state.i + 1 < state.src.size() && state.src[state.i + 1] == '*')
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        state.i += 2;
        state.curCol += 2;
        while (state.i < state.src.size() &&
               !(state.src[state.i] == '*' && state.i + 1 < state.src.size() && state.src[state.i + 1] == '/'))
        {
            if (state.src[state.i] == '\n')
            {
                state.curLine++;
                state.curCol = 0;
            }
            else
            {
                state.curCol++;
            }
            state.i++;
        }
        if (state.i < state.src.size())
        {
            state.i += 2;
            state.curCol += 2;
        }
        state.tokens.push_back({TokenType::BlockComment, state.src.substr(start, state.i - start), state.curLine,
                                startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

bool TryConsumePreprocessor(ScanState& state)
{
    char c = state.src[state.i];
    if (c == '#' && (state.curCol == 0 || state.tokens.empty() || state.newlines > 0))
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        while (state.i < state.src.size() && state.src[state.i] != '\r' && state.src[state.i] != '\n')
        {
            if (state.src[state.i] == '\\' && state.i + 1 < state.src.size() &&
                (state.src[state.i + 1] == '\r' || state.src[state.i + 1] == '\n'))
            {
                state.i +=
                    (state.src[state.i + 1] == '\r' && state.i + 2 < state.src.size() && state.src[state.i + 2] == '\n')
                        ? 3
                        : 2;
                state.curLine++;
                state.curCol = 0;
                continue;
            }
            state.i++;
            state.curCol++;
        }
        state.tokens.push_back({TokenType::Preprocessor, state.src.substr(start, state.i - start), state.curLine,
                                startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

/**
 * @brief Scans and emits an AngelScript multiline raw string literal delimited by triple quotes (`"""`), advancing
 * through the closing sequence or end of file while tracking line breaks.
 * @param[in,out] state Active lexer scanning state whose cursor index must point at the opening triple quote; on
 * success, advances past the closing quotes or to the end of input.
 * @return True if a triple-quoted raw string literal was detected and consumed; false if the opening quotes are absent,
 * leaving the scan state unmodified.
 * @note On success, advances the state cursor, updates line and column coordinates, resets state.newlines to zero, and
 * appends a TokenType::StringLiteral token referencing state.src.
 */
bool TryConsumeRawString(ScanState& state)
{
    char c = state.src[state.i];
    if (c == '"' && state.i + 2 < state.src.size() && state.src[state.i + 1] == '"' && state.src[state.i + 2] == '"')
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        state.i += 3;
        state.curCol += 3;
        while (state.i < state.src.size() && !(state.src[state.i] == '"' && state.i + 2 < state.src.size() &&
                                               state.src[state.i + 1] == '"' && state.src[state.i + 2] == '"'))
        {
            if (state.src[state.i] == '\n')
            {
                state.curLine++;
                state.curCol = 0;
            }
            else
            {
                state.curCol++;
            }
            state.i++;
        }
        if (state.i < state.src.size())
        {
            state.i += 3;
            state.curCol += 3;
        }
        state.tokens.push_back({TokenType::StringLiteral, state.src.substr(start, state.i - start), state.curLine,
                                startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

bool TryConsumeQuotedLiteral(ScanState& state, char quoteChar, TokenType tokenType)
{
    if (state.src[state.i] != quoteChar)
        return false;
    size_t start = state.i;
    uint32_t startCol = state.curCol;
    state.i++;
    state.curCol++;
    while (state.i < state.src.size() && state.src[state.i] != quoteChar)
    {
        if (state.src[state.i] == '\\' && state.i + 1 < state.src.size())
        {
            state.i += 2;
            state.curCol += 2;
            continue;
        }
        if (state.src[state.i] == '\n' || state.src[state.i] == '\r')
            break;
        state.i++;
        state.curCol++;
    }
    const bool closed = state.i < state.src.size() && state.src[state.i] == quoteChar;
    if (closed)
    {
        state.i++;
        state.curCol++;
    }
    Token tok{tokenType, state.src.substr(start, state.i - start), state.curLine, startCol, state.newlines};
    tok.isUnterminated = !closed;
    state.tokens.push_back(std::move(tok));
    state.newlines = 0;
    return true;
}
} // namespace angel_lsp::analysis::formatting
