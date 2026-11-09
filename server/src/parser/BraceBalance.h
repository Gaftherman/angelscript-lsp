#pragma once

#include <cstdint>
#include <string_view>

namespace angel_lsp::parser
{

/**
 * @brief Summary of active braces and unmatched positions across a source file.
 */
struct BraceBalance
{
    int openBraces = 0;
    int closeBraces = 0;
    int firstUnmatchedCloseLine = -1;
    uint32_t firstUnmatchedCloseByte = UINT32_MAX;

    [[nodiscard]] bool HasMissingOpenBrace() const noexcept
    {
        return openBraces < closeBraces;
    }
};

/**
 * @brief Advances index past a line or block comment, counting newlines inside block comments.
 * @param[in]
 * sourceCode Document source text.
 * @param[in,out] i Character index of the comment opener; left just past the
 * comment.
 * @param[in,out] currentLine Current line count.
 */
inline void SkipComment(std::string_view sourceCode, size_t& i, int& currentLine) noexcept
{
    const size_t len = sourceCode.size();
    if (sourceCode[i + 1] == '/')
    {
        i += 2;
        while (i < len && sourceCode[i] != '\n')
            ++i;
    }
    else if (sourceCode[i + 1] == '*')
    {
        i += 2;
        while (i + 1 < len && !(sourceCode[i] == '*' && sourceCode[i + 1] == '/'))
        {
            if (sourceCode[i] == '\n')
                ++currentLine;
            ++i;
        }
        i = (i + 1 < len) ? (i + 2) : len;
    }
}

/**
 * @brief Advances index past a triple-quoted heredoc literal (""" ... """).
 * @param[in] sourceCode Document source text.
 * @param[in,out] i Character index in source text.
 * @param[in,out] currentLine Current line count.
 */
inline void SkipHeredocLiteral(std::string_view sourceCode, size_t& i, int& currentLine) noexcept
{
    i += 3;
    const size_t len = sourceCode.size();
    while (i < len)
    {
        if (sourceCode[i] == '\n')
        {
            ++currentLine;
            ++i;
        }
        else if (sourceCode.substr(i).starts_with("\"\"\""))
        {
            i += 3;
            return;
        }
        else
        {
            ++i;
        }
    }
}

/**
 * @brief Advances index past a string or character literal, stopping at newlines for unclosed literals.
 * @param[in] sourceCode Document source text.
 * @param[in,out] i Character index in source text.
 * @param[in,out] currentLine Current line count.
 */
inline void SkipLiteral(std::string_view sourceCode, size_t& i, int& currentLine) noexcept
{
    if (sourceCode.substr(i).starts_with("\"\"\""))
    {
        SkipHeredocLiteral(sourceCode, i, currentLine);
        return;
    }
    const size_t len = sourceCode.size();
    const char quote = sourceCode[i++];
    while (i < len && sourceCode[i] != quote)
    {
        if (sourceCode[i] == '\\' && i + 1 < len)
        {
            if (sourceCode[i + 1] == '\n')
            {
                ++currentLine;
            }
            i += 2;
            continue;
        }
        if (sourceCode[i] == '\n' || sourceCode[i] == '\r')
        {
            break;
        }
        ++i;
    }
    if (i < len && sourceCode[i] == quote)
    {
        ++i;
    }
}

/**
 * @brief Fast single-pass calculation of brace balance outside comments and literals.
 * @param[in] sourceCode Document source text.
 * @return Computed BraceBalance metrics.
 */
[[nodiscard]] inline BraceBalance ComputeBraceBalance(std::string_view sourceCode) noexcept
{
    BraceBalance balance;
    int currentDepth = 0;
    int currentLine = 0;
    const size_t len = sourceCode.size();
    size_t i = 0;

    while (i < len)
    {
        const char c = sourceCode[i];
        if (c == '\n')
        {
            ++currentLine;
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < len && (sourceCode[i + 1] == '/' || sourceCode[i + 1] == '*'))
        {
            SkipComment(sourceCode, i, currentLine);
            continue;
        }
        if (c == '"' || c == '\'')
        {
            SkipLiteral(sourceCode, i, currentLine);
            continue;
        }
        if (c == '{')
        {
            ++balance.openBraces;
            ++currentDepth;
        }
        else if (c == '}')
        {
            ++balance.closeBraces;
            --currentDepth;
            if (currentDepth < 0 && balance.firstUnmatchedCloseLine == -1)
            {
                balance.firstUnmatchedCloseLine = currentLine;
                balance.firstUnmatchedCloseByte = static_cast<uint32_t>(i);
            }
        }
        ++i;
    }
    return balance;
}

} // namespace angel_lsp::parser
