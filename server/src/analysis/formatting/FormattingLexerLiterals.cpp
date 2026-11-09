#include "analysis/formatting/FormattingLexer.h"

#include <cctype>
#include <string_view>

namespace angel_lsp::analysis::formatting
{

/**
 * @brief Emits a `//` comment token running to the end of the line.
 * @param[in,out] state Scanner cursor; advanced past the comment on success.
 * @return True when a line comment started at the cursor.
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
 * @brief Emits a block comment token, tracking line breaks inside it.
 * @param[in,out] state Scanner cursor; advanced past the closing delimiter, or to the end of an unterminated comment.
 * @return True when a block comment started at the cursor.
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
 * @brief Emits a triple-quoted heredoc string token, which may span lines.
 * @param[in,out] state Scanner cursor; advanced past the closing quotes, or to the end of an unterminated literal.
 * @return True when a heredoc literal started at the cursor.
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
