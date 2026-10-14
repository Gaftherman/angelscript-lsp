#include "analysis/DocComment.h"
#include "analysis/DoxygenMarkdown.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/** @brief Splits text into line views up to maxLines, dropping a trailing carriage return from each. */
std::vector<std::string_view> SplitLinesUpTo(std::string_view str, size_t maxLines)
{
    std::vector<std::string_view> lines;
    lines.reserve(std::min(maxLines, size_t{256}));
    size_t start = 0;
    while (start < str.size() && lines.size() < maxLines)
    {
        size_t end = str.find('\n', start);
        if (end == std::string_view::npos)
        {
            std::string_view line = str.substr(start);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            lines.push_back(line);
            break;
        }
        std::string_view line = str.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

/** @brief Strips leading and trailing whitespace. */
std::string_view Trim(std::string_view str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::string ExtractLineComment(std::string_view rem)
{
    const size_t lpPos = rem.find("//@listpattern");
    if (lpPos != std::string_view::npos)
    {
        rem = rem.substr(0, lpPos);
    }
    while (!rem.empty() && (rem.back() == ' ' || rem.back() == '\t' || rem.back() == '\r' || rem.back() == '\n'))
    {
        rem.remove_suffix(1);
    }
    if (rem.empty() || rem == "//")
    {
        return "";
    }
    return std::string(rem);
}

std::string ExtractBlockComment(std::string_view line, size_t startPos)
{
    const size_t close = line.find("*/", startPos + 2);
    if (close != std::string_view::npos)
    {
        return std::string(line.substr(startPos, close + 2 - startPos));
    }
    return std::string(line.substr(startPos));
}

bool UpdateQuoteState(char c, bool& inString, bool& inChar)
{
    if (c == '"' && !inChar)
    {
        inString = !inString;
        return true;
    }
    if (c == '\'' && !inString)
    {
        inChar = !inChar;
        return true;
    }
    return false;
}

/**
 * @brief Extracts a trailing comment from a line of code.
 *
 * Skips string and character literals. Ignores synthetic internal `//@listpattern`.
 */
std::string ExtractTrailingComment(std::string_view line)
{
    bool inString = false;
    bool inChar = false;
    for (size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];
        if (c == '\\' && (inString || inChar))
        {
            ++i;
            continue;
        }
        if (UpdateQuoteState(c, inString, inChar))
        {
            continue;
        }
        if (!inString && !inChar && c == '/' && i + 1 < line.size())
        {
            if (line[i + 1] == '/')
            {
                return ExtractLineComment(line.substr(i));
            }
            if (line[i + 1] == '*')
            {
                return ExtractBlockComment(line, i);
            }
        }
    }
    return "";
}

std::vector<std::string_view> CollectBlockComment(const std::vector<std::string_view>& lines, int endLine)
{
    std::vector<std::string_view> commentLines;
    for (int line = endLine; line >= 0; --line)
    {
        std::string_view l = lines[line];
        commentLines.push_back(l);
        if (l.find("/*") != std::string_view::npos)
        {
            break;
        }
    }
    std::reverse(commentLines.begin(), commentLines.end());
    return commentLines;
}

std::vector<std::string_view> CollectLineComments(const std::vector<std::string_view>& lines, int endLine)
{
    std::vector<std::string_view> commentLines;
    for (int line = endLine; line >= 0; --line)
    {
        if (Trim(lines[line]).starts_with("//"))
        {
            commentLines.push_back(lines[line]);
        }
        else
        {
            break;
        }
    }
    std::reverse(commentLines.begin(), commentLines.end());
    return commentLines;
}

bool IsMetadataBlockLine(std::string_view trimmed)
{
    return trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
}

int FindPrecedingCommentLine(const std::vector<std::string_view>& lines, int startLine)
{
    int line = startLine;
    while (line >= 0 && IsMetadataBlockLine(Trim(lines[line])))
    {
        line--;
    }
    if (line < 0 || Trim(lines[line]).empty())
    {
        return -1;
    }
    return line;
}

std::vector<std::string_view> CollectPrecedingDocComments(const std::vector<std::string_view>& lines,
                                                          uint32_t declStartLine)
{
    if (declStartLine == 0)
    {
        return {};
    }
    const int candidateLine = FindPrecedingCommentLine(lines, static_cast<int>(declStartLine) - 1);
    if (candidateLine < 0)
    {
        return {};
    }
    const std::string_view trimmed = Trim(lines[candidateLine]);
    if (trimmed.ends_with("*/"))
    {
        return CollectBlockComment(lines, candidateLine);
    }
    if (trimmed.starts_with("//"))
    {
        return CollectLineComments(lines, candidateLine);
    }
    return {};
}
} // namespace

std::string ExtractDocComment(const std::string& sourceCode, uint32_t declStartLine)
{
    if (sourceCode.empty() ||
        (sourceCode.find("//") == std::string::npos && sourceCode.find("/*") == std::string::npos))
    {
        return "";
    }

    const auto lines = SplitLinesUpTo(sourceCode, static_cast<size_t>(declStartLine) + 1);
    if (declStartLine >= lines.size())
    {
        return "";
    }

    auto commentViews = CollectPrecedingDocComments(lines, declStartLine);
    std::string trailing;
    if (commentViews.empty())
    {
        trailing = ExtractTrailingComment(lines[declStartLine]);
        if (!trailing.empty())
        {
            commentViews.push_back(trailing);
        }
    }

    if (commentViews.empty())
    {
        return "";
    }

    // Return the RAW comment text: do not strip comment delimiters or leading stars here,
    // because tree-sitter-doxygen grammar matches the openers and handles leading stars.
    std::string rawComment;
    for (size_t i = 0; i < commentViews.size(); ++i)
    {
        if (i > 0)
        {
            rawComment += "\n";
        }
        rawComment += commentViews[i];
    }

    return RenderDoxygenMarkdown(rawComment);
}

bool HasPrecedingDocTagAtByte(std::string_view sourceCode, uint32_t declStartByte, std::string_view tag)
{
    if (sourceCode.empty() || tag.empty() || declStartByte >= sourceCode.size())
    {
        return false;
    }
    if (sourceCode.find(tag) == std::string_view::npos)
    {
        return false;
    }

    size_t pos = declStartByte;
    while (pos > 0)
    {
        size_t lineEnd = sourceCode.rfind('\n', pos - 1);
        size_t lineStart = (lineEnd == std::string_view::npos) ? 0 : lineEnd + 1;
        std::string_view line = sourceCode.substr(lineStart, pos - lineStart);
        std::string_view trimmed = Trim(line);
        if (trimmed.empty())
        {
            if (lineEnd == std::string_view::npos)
                break;
            pos = lineEnd;
            continue;
        }
        if (trimmed.starts_with("//") || trimmed.ends_with("*/") || trimmed.starts_with("/*") ||
            trimmed.starts_with("*"))
        {
            if (trimmed.find(tag) != std::string_view::npos)
            {
                return true;
            }
            if (lineEnd == std::string_view::npos)
                break;
            pos = lineEnd;
            continue;
        }
        break;
    }
    return false;
}

bool HasPrecedingDocTag(std::string_view sourceCode, uint32_t declStartLine, std::string_view tag)
{
    if (sourceCode.empty() || tag.empty() || declStartLine == 0)
    {
        return false;
    }
    if (sourceCode.find(tag) == std::string_view::npos)
    {
        return false;
    }

    size_t pos = 0;
    for (uint32_t line = 0; line < declStartLine && pos < sourceCode.size(); ++line)
    {
        size_t nextNl = sourceCode.find('\n', pos);
        if (nextNl == std::string_view::npos)
        {
            return false;
        }
        pos = nextNl + 1;
    }
    return HasPrecedingDocTagAtByte(sourceCode, static_cast<uint32_t>(pos), tag);
}
} // namespace angel_lsp::analysis
