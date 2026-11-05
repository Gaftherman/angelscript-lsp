#pragma once

#include "analysis/index/SymbolTable.h"

#include <concepts>
#include <cstdint>
#include <lsp/types.h>
#include <optional>
#include <string_view>

namespace angel_lsp::features::common
{

/**
 * @brief Converts any range-like structure with line and character fields into an LSP protocol Range.
 * @tparam T Structure type containing startLine, startCharacter, endLine, and endCharacter.
 * @param[in] item Source range object.
 * @return Converted LSP Range.
 */
template <typename T>
    requires requires(const T& t) {
        { t.startLine } -> std::convertible_to<uint32_t>;
        { t.startCharacter } -> std::convertible_to<uint32_t>;
        { t.endLine } -> std::convertible_to<uint32_t>;
        { t.endCharacter } -> std::convertible_to<uint32_t>;
    }
[[nodiscard]] constexpr lsp::Range ToLspRange(const T& item) noexcept
{
    return lsp::Range{lsp::Position{item.startLine, item.startCharacter},
                      lsp::Position{item.endLine, item.endCharacter}};
}

/**
 * @brief Checks if two LSP protocol ranges are identical in start and end coordinates.
 * @param[in] a First LSP range.
 * @param[in] b Second LSP range.
 * @return True if start and end positions match exactly; false otherwise.
 */
[[nodiscard]] constexpr bool SameRange(const lsp::Range& a, const lsp::Range& b) noexcept
{
    return a.start.line == b.start.line && a.start.character == b.start.character && a.end.line == b.end.line &&
           a.end.character == b.end.character;
}

/**
 * @brief Compares two ranges or coordinate spans by start line and character.
 * @tparam T Range type containing start position.
 * @param[in] a First range object.
 * @param[in] b Second range object.
 * @return True if a strictly precedes b in document order.
 */
template <typename T> [[nodiscard]] constexpr bool CompareByPosition(const T& a, const T& b) noexcept
{
    if (a.start.line != b.start.line)
    {
        return a.start.line < b.start.line;
    }
    return a.start.character < b.start.character;
}

/**
 * @brief Checks if a character belongs to an AngelScript identifier.
 * @param[in] c Character byte.
 * @return True if alphanumeric or underscore.
 */
[[nodiscard]] constexpr bool IsIdentifierChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/**
 * @brief Extracts the full text view of a specific line from source code.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] targetLine 0-indexed line number.
 * @return View of line characters without newline terminators.
 */
[[nodiscard]] inline std::string_view ExtractLineAt(std::string_view sourceCode, uint32_t targetLine) noexcept
{
    uint32_t currentLine = 0;
    size_t lineStart = 0;
    while (currentLine < targetLine && lineStart < sourceCode.size())
    {
        const size_t nextNewline = sourceCode.find('\n', lineStart);
        if (nextNewline == std::string_view::npos)
        {
            return "";
        }
        lineStart = nextNewline + 1;
        currentLine++;
    }
    if (currentLine != targetLine || lineStart >= sourceCode.size())
    {
        return "";
    }
    size_t lineEnd = sourceCode.find('\n', lineStart);
    if (lineEnd == std::string_view::npos)
    {
        lineEnd = sourceCode.size();
    }
    if (lineEnd > lineStart && sourceCode[lineEnd - 1] == '\r')
    {
        lineEnd--;
    }
    return sourceCode.substr(lineStart, lineEnd - lineStart);
}

/**
 * @brief Computes 0-indexed byte offset of line start in document text.
 * @param[in] sourceCode Document source text.
 * @param[in] targetLine 0-indexed line number.
 * @return Byte offset of line start, or sourceCode.size() if out of bounds.
 */
[[nodiscard]] inline size_t ExtractLineStartByte(std::string_view sourceCode, uint32_t targetLine) noexcept
{
    size_t lineStart = 0;
    uint32_t currentLine = 0;
    while (currentLine < targetLine && lineStart < sourceCode.size())
    {
        size_t nextNewline = sourceCode.find('\n', lineStart);
        if (nextNewline == std::string_view::npos)
        {
            return sourceCode.size();
        }
        lineStart = nextNewline + 1;
        currentLine++;
    }
    return currentLine == targetLine ? lineStart : sourceCode.size();
}

/**
 * @brief Skips single-line comment bytes.
 * @param[in] sourceCode Document source text.
 * @param[in] i Starting index of comment '//'.
 * @return Index after comment or end of string.
 */
[[nodiscard]] inline size_t SkipLineComment(std::string_view sourceCode, size_t i) noexcept
{
    i += 2;
    while (i < sourceCode.size() && sourceCode[i] != '\n')
    {
        ++i;
    }
    return i;
}

/**
 * @brief Skips block comment bytes.
 * @param[in] sourceCode Document source text.
 * @param[in] i Starting index of block comment slash-star.
 * @return Index after block comment or end of string.
 */
[[nodiscard]] inline size_t SkipBlockComment(std::string_view sourceCode, size_t i) noexcept
{
    i += 2;
    while (i + 1 < sourceCode.size() && !(sourceCode[i] == '*' && sourceCode[i + 1] == '/'))
    {
        ++i;
    }
    return (i + 1 < sourceCode.size()) ? i + 2 : sourceCode.size();
}

/**
 * @brief Skips quoted string or character literal bytes.
 * @param[in] sourceCode Document source text.
 * @param[in] i Starting index of opening quote.
 * @return Index after closing quote or end of string.
 */
[[nodiscard]] inline size_t SkipQuotedLiteral(std::string_view sourceCode, size_t i) noexcept
{
    if (sourceCode[i] == '@' && i + 1 < sourceCode.size() && (sourceCode[i + 1] == '"' || sourceCode[i + 1] == '\''))
    {
        const char quote = sourceCode[i + 1];
        i += 2;
        while (i < sourceCode.size())
        {
            if (sourceCode[i] == quote)
            {
                if (i + 1 < sourceCode.size() && sourceCode[i + 1] == quote)
                {
                    i += 2;
                    continue;
                }
                ++i;
                break;
            }
            ++i;
        }
        return i;
    }

    if (sourceCode[i] == '"' && i + 2 < sourceCode.size() && sourceCode[i + 1] == '"' && sourceCode[i + 2] == '"')
    {
        i += 3;
        while (i + 2 < sourceCode.size() &&
               !(sourceCode[i] == '"' && sourceCode[i + 1] == '"' && sourceCode[i + 2] == '"'))
        {
            ++i;
        }
        return (i + 2 < sourceCode.size()) ? i + 3 : sourceCode.size();
    }

    char quote = sourceCode[i];
    ++i;
    while (i < sourceCode.size() && sourceCode[i] != quote && sourceCode[i] != '\n')
    {
        if (sourceCode[i] == '\\' && i + 1 < sourceCode.size())
        {
            i += 2;
        }
        else
        {
            ++i;
        }
    }
    if (i < sourceCode.size() && sourceCode[i] == quote)
    {
        ++i;
    }
    return i;
}

/**
 * @brief Checks if a byte position resides inside a comment or string literal.
 * @param[in] sourceCode Document source text.
 * @param[in] targetByte Byte offset within source text.
 * @return True if offset is inside a line comment, block comment, or string.
 */
[[nodiscard]] inline bool IsInsideCommentOrString(std::string_view sourceCode, size_t targetByte) noexcept
{
    if (targetByte >= sourceCode.size())
    {
        return false;
    }

    size_t i = 0;
    while (i < targetByte)
    {
        char c = sourceCode[i];
        if (c == '/' && i + 1 < sourceCode.size())
        {
            if (sourceCode[i + 1] == '/')
            {
                size_t commentStart = i;
                i = SkipLineComment(sourceCode, i);
                if (targetByte >= commentStart && targetByte < i)
                {
                    return true;
                }
                continue;
            }
            if (sourceCode[i + 1] == '*')
            {
                size_t commentStart = i;
                i = SkipBlockComment(sourceCode, i);
                if (targetByte >= commentStart && targetByte < i)
                {
                    return true;
                }
                continue;
            }
        }
        else if (c == '"' || c == '\'' ||
                 (c == '@' && i + 1 < sourceCode.size() && (sourceCode[i + 1] == '"' || sourceCode[i + 1] == '\'')))
        {
            size_t strStart = i;
            i = SkipQuotedLiteral(sourceCode, i);
            if (targetByte >= strStart && targetByte < i)
            {
                return true;
            }
            continue;
        }
        ++i;
    }
    return false;
}

/**
 * @brief Expands start column to encompass a full scoped identifier.
 * @param[in] line Source line text.
 * @param[in] col Column position.
 * @return Start column index.
 */
[[nodiscard]] inline size_t ExpandScopedIdentifierStart(std::string_view line, size_t col) noexcept
{
    size_t start = col;
    while (start > 0 && IsIdentifierChar(line[start - 1]))
    {
        start--;
    }
    while (start >= 2 && line[start - 1] == ':' && line[start - 2] == ':')
    {
        size_t segStart = start - 2;
        while (segStart > 0 && IsIdentifierChar(line[segStart - 1]))
        {
            segStart--;
        }
        if (segStart == start - 2)
        {
            break;
        }
        start = segStart;
    }
    return start;
}

/**
 * @brief Expands end column to encompass the current scoped identifier segment.
 * @param[in] line Source line text.
 * @param[in] col Column position.
 * @return End column index of current segment.
 */
[[nodiscard]] inline size_t ExpandScopedIdentifierEnd(std::string_view line, size_t col) noexcept
{
    size_t end = col;
    while (end < line.size() && IsIdentifierChar(line[end]))
    {
        end++;
    }
    return end;
}

/**
 * @brief Expands end column to encompass the full scoped identifier including forward segments.
 * @param[in] line Source line text.
 * @param[in] col Column position.
 * @return End column index of entire qualified chain.
 */
[[nodiscard]] inline size_t ExpandFullScopedIdentifierEnd(std::string_view line, size_t col) noexcept
{
    size_t end = col;
    while (end < line.size() && IsIdentifierChar(line[end]))
    {
        end++;
    }
    while (end + 2 <= line.size() && line[end] == ':' && line[end + 1] == ':')
    {
        size_t segEnd = end + 2;
        while (segEnd < line.size() && IsIdentifierChar(line[segEnd]))
        {
            segEnd++;
        }
        if (segEnd == end + 2)
        {
            break;
        }
        end = segEnd;
    }
    return end;
}

/**
 * @brief Finds the identifier text located at an LSP cursor position without requiring an AST.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] position Cursor line and column.
 * @return Extracted identifier text view or empty view if not on an identifier.
 */
[[nodiscard]] inline std::string_view FindIdentifierAtPosition(std::string_view sourceCode,
                                                               lsp::Position position) noexcept
{
    if (sourceCode.empty())
    {
        return "";
    }
    const std::string_view line = ExtractLineAt(sourceCode, position.line);
    if (line.empty())
    {
        return "";
    }
    size_t col = position.character;
    if (col >= line.size() || !IsIdentifierChar(line[col]))
    {
        if (col > 0 && IsIdentifierChar(line[col - 1]))
        {
            col--;
        }
        else
        {
            return "";
        }
    }
    const size_t lineStart = static_cast<size_t>(line.data() - sourceCode.data());
    if (IsInsideCommentOrString(sourceCode, lineStart + col))
    {
        return "";
    }
    size_t start = col;
    while (start > 0 && IsIdentifierChar(line[start - 1]))
    {
        start--;
    }
    size_t end = col;
    while (end < line.size() && IsIdentifierChar(line[end]))
    {
        end++;
    }
    return line.substr(start, end - start);
}

/**
 * @brief Finds the line-relative range of an identifier located at an LSP cursor position.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] position Cursor line and column.
 * @return Extracted identifier Range or nullopt if not on an identifier.
 */
[[nodiscard]] inline std::optional<lsp::Range> FindIdentifierRangeAtPosition(std::string_view sourceCode,
                                                                             lsp::Position position) noexcept
{
    if (sourceCode.empty())
    {
        return std::nullopt;
    }
    const std::string_view line = ExtractLineAt(sourceCode, position.line);
    if (line.empty())
    {
        return std::nullopt;
    }
    size_t col = position.character;
    if (col >= line.size() || !IsIdentifierChar(line[col]))
    {
        if (col > 0 && IsIdentifierChar(line[col - 1]))
        {
            col--;
        }
        else
        {
            return std::nullopt;
        }
    }
    const size_t lineStart = static_cast<size_t>(line.data() - sourceCode.data());
    if (IsInsideCommentOrString(sourceCode, lineStart + col))
    {
        return std::nullopt;
    }
    size_t start = col;
    while (start > 0 && IsIdentifierChar(line[start - 1]))
    {
        start--;
    }
    size_t end = col;
    while (end < line.size() && IsIdentifierChar(line[end]))
    {
        end++;
    }
    return lsp::Range{
        lsp::Position{position.line, static_cast<uint32_t>(start)},
        lsp::Position{position.line, static_cast<uint32_t>(end)},
    };
}

/**
 * @brief Finds the scoped identifier text located at an LSP cursor position without requiring an AST.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] position Cursor line and column.
 * @return Extracted qualified identifier text view or empty view.
 */
[[nodiscard]] inline std::string_view FindScopedIdentifierAtPosition(std::string_view sourceCode,
                                                                     lsp::Position position) noexcept
{
    const std::string_view ident = FindIdentifierAtPosition(sourceCode, position);
    if (ident.empty())
    {
        return "";
    }
    const std::string_view line = ExtractLineAt(sourceCode, position.line);
    if (line.empty())
    {
        return ident;
    }
    size_t col = position.character;
    if (col >= line.size() || !IsIdentifierChar(line[col]))
    {
        if (col > 0 && IsIdentifierChar(line[col - 1]))
        {
            col--;
        }
        else
        {
            return ident;
        }
    }
    const size_t start = ExpandScopedIdentifierStart(line, col);
    const size_t end = ExpandScopedIdentifierEnd(line, col);
    return line.substr(start, end - start);
}

/**
 * @brief Finds the full scoped identifier text expanding both backward and forward across ::.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] position Cursor line and column.
 * @return Extracted full qualified identifier text view or empty view.
 */
[[nodiscard]] inline std::string_view FindFullScopedIdentifierAtPosition(std::string_view sourceCode,
                                                                         lsp::Position position) noexcept
{
    const std::string_view ident = FindIdentifierAtPosition(sourceCode, position);
    if (ident.empty())
    {
        return "";
    }
    const std::string_view line = ExtractLineAt(sourceCode, position.line);
    if (line.empty())
    {
        return ident;
    }
    size_t col = position.character;
    if (col >= line.size() || !IsIdentifierChar(line[col]))
    {
        if (col > 0 && IsIdentifierChar(line[col - 1]))
        {
            col--;
        }
        else
        {
            return ident;
        }
    }
    const size_t start = ExpandScopedIdentifierStart(line, col);
    const size_t end = ExpandFullScopedIdentifierEnd(line, col);
    return line.substr(start, end - start);
}

/**
 * @brief Maps internal AngelScript SymbolType to corresponding LSP SymbolKind.
 * @param[in] type Internal SymbolType enum value.
 * @param[in] isMember True if symbol is contained in a class or struct.
 * @return Equivalent LSP SymbolKind.
 */
[[nodiscard]] lsp::SymbolKind ToLspSymbolKind(analysis::SymbolType type, bool isMember = false) noexcept;

} // namespace angel_lsp::features::common
