#include "analysis/DocComment.h"
#include "analysis/DoxygenMarkdown.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/** @brief Splits text into lines, dropping a trailing carriage return from each. */
std::vector<std::string> SplitLines(const std::string& str)
{
    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(str);
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

/** @brief Strips leading and trailing whitespace. */
std::string Trim(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
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

std::string ExtractBlockComment(const std::string& line, size_t startPos)
{
    const size_t close = line.find("*/", startPos + 2);
    if (close != std::string::npos)
    {
        return line.substr(startPos, close + 2 - startPos);
    }
    return line.substr(startPos);
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
std::string ExtractTrailingComment(const std::string& line)
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
                return ExtractLineComment(std::string_view(line).substr(i));
            }
            if (line[i + 1] == '*')
            {
                return ExtractBlockComment(line, i);
            }
        }
    }
    return "";
}

int FindPrecedingCommentLine(const std::vector<std::string>& lines, int startLine)
{
    int line = startLine;
    while (line >= 0 && Trim(lines[line]).empty())
    {
        line--;
    }
    return line;
}

std::vector<std::string> CollectBlockComment(const std::vector<std::string>& lines, int endLine)
{
    std::vector<std::string> commentLines;
    for (int line = endLine; line >= 0; --line)
    {
        const std::string& l = lines[line];
        commentLines.push_back(l);
        if (l.find("/*") != std::string::npos)
        {
            break;
        }
    }
    std::reverse(commentLines.begin(), commentLines.end());
    return commentLines;
}

std::vector<std::string> CollectLineComments(const std::vector<std::string>& lines, int endLine)
{
    std::vector<std::string> commentLines;
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

std::vector<std::string> CollectPrecedingDocComments(const std::vector<std::string>& lines, uint32_t declStartLine)
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
    const std::string trimmed = Trim(lines[candidateLine]);
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
    if (sourceCode.empty())
    {
        return "";
    }

    const auto lines = SplitLines(sourceCode);
    if (declStartLine >= lines.size())
    {
        return "";
    }

    std::vector<std::string> commentLines = CollectPrecedingDocComments(lines, declStartLine);
    if (commentLines.empty())
    {
        std::string trailing = ExtractTrailingComment(lines[declStartLine]);
        if (!trailing.empty())
        {
            commentLines.push_back(std::move(trailing));
        }
    }

    if (commentLines.empty())
    {
        return "";
    }

    // Return the RAW comment text: do not strip comment delimiters or leading stars here,
    // because tree-sitter-doxygen grammar matches the openers and handles leading stars.
    std::string rawComment;
    for (size_t i = 0; i < commentLines.size(); ++i)
    {
        if (i > 0)
        {
            rawComment += "\n";
        }
        rawComment += commentLines[i];
    }

    return RenderDoxygenMarkdown(rawComment);
}
} // namespace angel_lsp::analysis
