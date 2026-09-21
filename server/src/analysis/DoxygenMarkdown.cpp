#include "analysis/DoxygenMarkdown.h"
#include "parser/DoxygenParser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/** @brief Strips leading and trailing ASCII whitespace. */
std::string Trim(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

/** @brief Strips leading ASCII whitespace. */
std::string TrimLeading(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return "";
    }
    return str.substr(first);
}

/** @brief Strips trailing ASCII whitespace in place. */
void TrimTrailing(std::string& str)
{
    size_t last = str.find_last_not_of(" \t\r\n");
    if (last == std::string::npos)
    {
        str.clear();
    }
    else
    {
        str.erase(last + 1);
    }
}

/** @brief Converts ASCII string to lowercase. */
std::string ToLower(const std::string& str)
{
    std::string out = str;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/** @brief Splits text into lines, stripping trailing carriage returns. */
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
        lines.push_back(std::move(line));
    }
    return lines;
}

/**
 * @brief Strips leading comment star decoration [ \t]*\*[ \t]? from a line.
 *
 * Required for lines after the first in multi-line descriptions and code blocks.
 */
std::string StripLineLeadingStar(const std::string& line)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        ++i;
    }
    if (i < line.size() && line[i] == '*')
    {
        ++i;
        if (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        {
            ++i;
        }
    }
    return line.substr(i);
}

/** @brief Checks if a cleaned line represents a Markdown bullet or numbered list item. */
bool IsMarkdownListLine(std::string_view line)
{
    if (line.starts_with("* ") || line.starts_with("- "))
    {
        return true;
    }
    return line.size() >= 3 && std::isdigit(static_cast<unsigned char>(line[0])) &&
           line.find(". ") != std::string_view::npos;
}

/** @brief Transforms `-# ` Doxygen numbered list items into numbered Markdown list items. */
void ProcessNumberedListItem(std::string& line, int& listNum)
{
    if (line.starts_with("-# ") || line.starts_with("-#\t"))
    {
        line = std::to_string(listNum++) + ". " + TrimLeading(line.substr(2));
    }
    else
    {
        listNum = 1;
    }
}

/**
 * @brief Cleans description lines by stripping line decorations, converting `-#` list items,
 * and joining lines with single spaces or linebreaks (for lists).
 */
std::string CleanDescriptionLines(const std::string& raw)
{
    auto lines = SplitLines(raw);
    if (lines.empty())
    {
        return "";
    }

    std::vector<std::string> cleaned;
    int listNum = 1;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string l = lines[i];
        if (i > 0)
        {
            l = StripLineLeadingStar(l);
        }
        l = Trim(l);
        if (!l.empty())
        {
            ProcessNumberedListItem(l, listNum);
            cleaned.push_back(std::move(l));
        }
    }

    std::string result;
    for (size_t i = 0; i < cleaned.size(); ++i)
    {
        if (i > 0)
        {
            bool isListLine = IsMarkdownListLine(cleaned[i]);
            bool prevIsListLine = IsMarkdownListLine(cleaned[i - 1]);
            result += (isListLine || prevIsListLine) ? "\n" : " ";
        }
        result += cleaned[i];
    }
    return Trim(result);
}

/**
 * @brief Converts common inline HTML tags (<code>, <tt>, <b>, <strong>, <i>, <em>) to Markdown.
 */
std::string RewriteInlineHtml(const std::string& text)
{
    if (text.find('<') == std::string::npos)
    {
        return text;
    }

    std::string result;
    result.reserve(text.size());
    size_t i = 0;
    while (i < text.size())
    {
        if (text[i] == '<')
        {
            std::string_view rem(&text[i], text.size() - i);
            auto tryReplaceTag = [&](std::string_view openTag, std::string_view closeTag,
                                     std::string_view mdWrapper) -> bool
            {
                if (rem.starts_with(openTag))
                {
                    size_t closePos = text.find(closeTag, i + openTag.size());
                    if (closePos != std::string::npos)
                    {
                        std::string inner = text.substr(i + openTag.size(), closePos - (i + openTag.size()));
                        result += mdWrapper;
                        result += inner;
                        result += mdWrapper;
                        i = closePos + closeTag.size();
                        return true;
                    }
                }
                return false;
            };

            if (tryReplaceTag("<code>", "</code>", "`") || tryReplaceTag("<tt>", "</tt>", "`") ||
                tryReplaceTag("<b>", "</b>", "**") || tryReplaceTag("<strong>", "</strong>", "**") ||
                tryReplaceTag("<i>", "</i>", "*") || tryReplaceTag("<em>", "</em>", "*"))
            {
                continue;
            }
        }
        result += text[i];
        ++i;
    }
    return result;
}

/** @brief Checks if character is a Doxygen escaped character. */
bool IsDoxygenEscapedChar(char c)
{
    return c == '@' || c == '\\' || c == '$' || c == '&' || c == '#' || c == '%' || c == '<' || c == '>';
}

/** @brief Tries to consume a Doxygen escape sequence (\@, \\, @@, etc.). */
bool TryConsumeInlineEscape(std::string_view text, size_t& i, std::string& result)
{
    if (text[i] == '\\' && i + 1 < text.size() && IsDoxygenEscapedChar(text[i + 1]))
    {
        result += text[i + 1];
        i += 2;
        return true;
    }
    if (text[i] == '@' && i + 1 < text.size() && text[i + 1] == '@')
    {
        result += '@';
        i += 2;
        return true;
    }
    return false;
}

/** @brief Checks if a character precedes an inline command as whitespace or opening punctuation. */
bool IsInlineCommandPrefix(std::string_view text, size_t i)
{
    if (i == 0)
    {
        return true;
    }
    char prev = text[i - 1];
    return std::isspace(static_cast<unsigned char>(prev)) || prev == '(' || prev == '[' || prev == '{' || prev == '<' ||
           prev == '"' || prev == '\'';
}

/** @brief Checks if trailing character is punctuation that should remain outside formatting. */
bool IsTrailingPunctuation(char c)
{
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == ')' || c == ']' || c == '}';
}

/** @brief Result of matching an inline formatting command. */
struct MatchedInlineCommand
{
    size_t cmdLen = 0;
    std::string_view cmd;
};

/** @brief Matches known inline commands (ref, em, c, p, b, a, e) at position i. */
MatchedInlineCommand MatchInlineCommandName(std::string_view text, size_t i)
{
    if (i + 4 <= text.size() && text.substr(i + 1, 3) == "ref")
    {
        return {3, "ref"};
    }
    if (i + 3 <= text.size() && text.substr(i + 1, 2) == "em")
    {
        return {2, "em"};
    }
    if (i + 2 <= text.size())
    {
        char c = text[i + 1];
        if (c == 'c' || c == 'p' || c == 'b' || c == 'a' || c == 'e')
        {
            return {1, text.substr(i + 1, 1)};
        }
    }
    return {0, {}};
}

/** @brief Formats an inline command and its argument into Markdown. */
std::string FormatInlineCommand(std::string_view cmd, std::string_view arg)
{
    if (cmd == "c" || cmd == "p" || cmd == "ref")
    {
        return "`" + std::string(arg) + "`";
    }
    if (cmd == "b")
    {
        return "**" + std::string(arg) + "**";
    }
    return "*" + std::string(arg) + "*";
}

/** @brief Trims trailing punctuation from argument slice [argStart, argEnd). */
size_t TrimArgTrailingPunctuation(std::string_view text, size_t argStart, size_t argEnd)
{
    size_t trailingStart = argEnd;
    while (trailingStart > argStart && IsTrailingPunctuation(text[trailingStart - 1]))
    {
        --trailingStart;
    }
    return trailingStart > argStart ? trailingStart : argEnd;
}

/** @brief Tries to rewrite an inline command at position i. */
bool TryRewriteInlineCommand(const std::string& text, size_t& i, std::string& result)
{
    if (text[i] != '@' && text[i] != '\\')
    {
        return false;
    }
    if (!IsInlineCommandPrefix(text, i))
    {
        return false;
    }

    auto match = MatchInlineCommandName(text, i);
    if (match.cmdLen == 0 || i + 1 + match.cmdLen >= text.size() ||
        !std::isspace(static_cast<unsigned char>(text[i + 1 + match.cmdLen])))
    {
        return false;
    }

    size_t argStart = i + 1 + match.cmdLen;
    while (argStart < text.size() && (text[argStart] == ' ' || text[argStart] == '\t'))
    {
        ++argStart;
    }
    if (argStart >= text.size() || std::isspace(static_cast<unsigned char>(text[argStart])))
    {
        return false;
    }

    size_t argEnd = argStart;
    while (argEnd < text.size() && !std::isspace(static_cast<unsigned char>(text[argEnd])))
    {
        ++argEnd;
    }

    argEnd = TrimArgTrailingPunctuation(text, argStart, argEnd);
    std::string_view arg(&text[argStart], argEnd - argStart);
    result += FormatInlineCommand(match.cmd, arg);
    i = argEnd;
    return true;
}

/**
 * @brief Rewrites at-form inline formatting commands (@c, @p, @b, @a, @e, @em, @ref) in verbatim text.
 */
std::string RewriteAtInlineCommands(const std::string& input)
{
    if (input.empty())
    {
        return "";
    }

    std::string text = RewriteInlineHtml(input);
    std::string result;
    result.reserve(text.size());
    size_t i = 0;
    while (i < text.size())
    {
        if (TryConsumeInlineEscape(text, i, result))
        {
            continue;
        }
        if (TryRewriteInlineCommand(text, i, result))
        {
            continue;
        }
        result += text[i];
        ++i;
    }
    return result;
}

/**
 * @brief Checks if a token is a known Doxygen command starting with 'n'.
 */
bool IsKnownDoxygenCommandStartingWithN(std::string_view text)
{
    auto matchesCmd = [&](std::string_view cmd)
    {
        if (text.size() < cmd.size())
        {
            return false;
        }
        for (size_t i = 0; i < cmd.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(text[i])) != cmd[i])
            {
                return false;
            }
        }
        if (text.size() == cmd.size())
        {
            return true;
        }
        return !std::isalpha(static_cast<unsigned char>(text[cmd.size()]));
    };

    return matchesCmd("note") || matchesCmd("name") || matchesCmd("namespace") || matchesCmd("nosubgrouping");
}

/** @brief Checks whether position i in line is an unescaped literal newline command. */
bool IsEscapedNewline(const std::string& line, size_t i)
{
    if ((line[i] != '\\' && line[i] != '@') || i + 1 >= line.size() || line[i + 1] != 'n')
    {
        return false;
    }
    if (line[i] == '\\' && i > 0 && line[i - 1] == '\\')
    {
        return false;
    }
    return !IsKnownDoxygenCommandStartingWithN(std::string_view(line).substr(i + 1));
}

/**
 * @brief Splits a comment line into multiple lines at literal \n or @n escapes.
 */
std::vector<std::string> SplitCommentLineOnNewlines(const std::string& line)
{
    std::vector<std::string> result;
    size_t start = 0;
    size_t i = 0;
    while (i < line.size())
    {
        if (line[i] == '\\' && i > 0 && line[i - 1] == '\\')
        {
            ++i;
            continue;
        }

        if (IsEscapedNewline(line, i))
        {
            size_t chunkEnd = (i >= 2 && line[i - 2] == '\\' && line[i - 1] == 'r') ? i - 2 : i;
            if (chunkEnd < start)
            {
                chunkEnd = start;
            }
            result.push_back(line.substr(start, chunkEnd - start));
            i += 2;
            if (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            {
                ++i;
            }
            start = i;
            continue;
        }
        ++i;
    }
    result.push_back(line.substr(start));
    return result;
}

/** @brief Tries to process opening line of a C-style block comment. Returns true if block remains open. */
bool TryProcessFirstBlockLine(const std::string& line, std::vector<std::string>& contents)
{
    size_t openPos = line.find("/*");
    if (openPos == std::string::npos)
    {
        return false;
    }
    size_t start = openPos + 2;
    size_t closePos = line.rfind("*/");
    if (closePos != std::string::npos && closePos >= start)
    {
        while (start < closePos && (line[start] == '*' || line[start] == '!'))
        {
            ++start;
        }
        std::string inner = Trim(line.substr(start, closePos - start));
        if (!inner.empty())
        {
            contents.push_back(std::move(inner));
        }
        return false;
    }

    while (start < line.size() && (line[start] == '*' || line[start] == '!'))
    {
        ++start;
    }
    std::string afterOpen = line.substr(start);
    if (!Trim(afterOpen).empty())
    {
        if (!afterOpen.empty() && afterOpen.front() == ' ')
        {
            afterOpen = afterOpen.substr(1);
        }
        contents.push_back(std::move(afterOpen));
    }
    return true;
}

/** @brief Extracts content lines from a multi-line C-style comment block. */
std::vector<std::string> ExtractBlockCommentLines(const std::vector<std::string>& rawLines)
{
    std::vector<std::string> contents;
    bool inBlock = false;
    for (const auto& line : rawLines)
    {
        if (!inBlock)
        {
            inBlock = TryProcessFirstBlockLine(line, contents);
            continue;
        }

        size_t closePos = line.find("*/");
        if (closePos != std::string::npos)
        {
            std::string beforeClose = StripLineLeadingStar(line.substr(0, closePos));
            TrimTrailing(beforeClose);
            if (!beforeClose.empty())
            {
                contents.push_back(std::move(beforeClose));
            }
            break;
        }

        contents.push_back(StripLineLeadingStar(line));
    }
    return contents;
}

/** @brief Extracts content lines from single-line C++ comment runs. */
std::vector<std::string> ExtractLineCommentLines(const std::vector<std::string>& rawLines)
{
    std::vector<std::string> contents;
    for (const auto& line : rawLines)
    {
        size_t pos = line.find("//");
        if (pos == std::string::npos)
        {
            continue;
        }
        std::string sub = line.substr(pos + 2);
        while (sub.starts_with('/') || sub.starts_with('!'))
        {
            sub = sub.substr(1);
        }
        if (!sub.empty() && sub.front() == ' ')
        {
            sub = sub.substr(1);
        }
        contents.push_back(std::move(sub));
    }
    return contents;
}

/** @brief Splits comment contents on newline escapes outside verbatim blocks. */
std::vector<std::string> SplitCommentVerbatimAndNewlines(const std::vector<std::string>& contents)
{
    std::vector<std::string> finalContents;
    finalContents.reserve(contents.size());
    bool inVerbatim = false;
    for (const auto& cLine : contents)
    {
        std::string trimmedLine = TrimLeading(cLine);
        if (trimmedLine.starts_with("@code") || trimmedLine.starts_with("\\code") ||
            trimmedLine.starts_with("@verbatim") || trimmedLine.starts_with("\\verbatim"))
        {
            inVerbatim = true;
            finalContents.push_back(cLine);
            continue;
        }
        if (trimmedLine.starts_with("@endcode") || trimmedLine.starts_with("\\endcode") ||
            trimmedLine.starts_with("@endverbatim") || trimmedLine.starts_with("\\endverbatim"))
        {
            inVerbatim = false;
            finalContents.push_back(cLine);
            continue;
        }

        if (inVerbatim)
        {
            finalContents.push_back(cLine);
        }
        else
        {
            auto splitLines = SplitCommentLineOnNewlines(cLine);
            for (auto&& s : splitLines)
            {
                finalContents.push_back(std::move(s));
            }
        }
    }
    return finalContents;
}

/**
 * @brief Extracts the content lines of a raw comment, whichever way it was written.
 */
std::vector<std::string> ExtractCommentLines(const std::string& rawComment)
{
    std::string trimmed = Trim(rawComment);
    if (trimmed.empty())
    {
        return {};
    }

    auto rawLines = SplitLines(rawComment);
    auto contents = trimmed.starts_with("/*") ? ExtractBlockCommentLines(rawLines) : ExtractLineCommentLines(rawLines);
    return SplitCommentVerbatimAndNewlines(contents);
}

enum class CommentSegmentKind
{
    Tag,
    Text,
    VerbatimCode
};

struct CommentSegment
{
    CommentSegmentKind kind = CommentSegmentKind::Text;
    std::vector<std::string> lines;
    std::string language;
};

/** @brief Checks if a command name is an inline formatting command. */
bool IsDoxygenInlineCommandName(std::string_view cmd)
{
    return cmd == "b" || cmd == "e" || cmd == "em" || cmd == "a" || cmd == "c" || cmd == "p" || cmd == "n";
}

/** @brief Checks if a command name is a verbatim code block boundary command. */
bool IsDoxygenVerbatimCommandName(std::string_view cmd)
{
    return cmd == "code" || cmd == "endcode" || cmd == "verbatim" || cmd == "endverbatim";
}

/**
 * @brief Checks if a line begins with a Doxygen block command (@name or \name).
 */
bool IsBlockCommand(std::string_view line, size_t& cmdEnd)
{
    if (line.size() < 2 || (line[0] != '@' && line[0] != '\\') || !std::isalpha(static_cast<unsigned char>(line[1])))
    {
        return false;
    }

    size_t i = 1;
    while (i < line.size() && std::isalpha(static_cast<unsigned char>(line[i])))
    {
        ++i;
    }

    std::string cmdName = ToLower(std::string(line.substr(1, i - 1)));
    if (IsDoxygenInlineCommandName(cmdName) || IsDoxygenVerbatimCommandName(cmdName))
    {
        return false;
    }

    cmdEnd = i;
    return true;
}

/** @brief Checks if a character is a closing quotation mark or bracket. */
bool IsClosingQuoteOrBracket(char c)
{
    return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}';
}

/** @brief Checks if dot at position i marks a sentence boundary. */
bool IsDotSentenceBoundary(std::string_view line, size_t i)
{
    if (i > 0 && (line[i - 1] == '.' || std::isdigit(static_cast<unsigned char>(line[i - 1]))))
    {
        return false;
    }
    if (i + 1 < line.size() && line[i + 1] == '.')
    {
        return false;
    }

    size_t next = i + 1;
    while (next < line.size() && (IsClosingQuoteOrBracket(line[next]) || line[next] == ';'))
    {
        ++next;
    }
    return next == line.size() || std::isspace(static_cast<unsigned char>(line[next]));
}

/** @brief Finds the first sentence-ending dot in a line not part of numbers or ellipsis. */
size_t FindSentenceEndingDot(std::string_view line)
{
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '.' && IsDotSentenceBoundary(line, i))
        {
            return i;
        }
    }
    return std::string::npos;
}

/** @brief Checks if a line starts with an explicit brief command. */
bool IsExplicitBriefStart(std::string_view trimmed)
{
    if (trimmed.starts_with("@brief") || trimmed.starts_with("\\brief") || trimmed.starts_with("@short") ||
        trimmed.starts_with("\\short"))
    {
        return trimmed.size() == 6 || std::isspace(static_cast<unsigned char>(trimmed[6]));
    }
    return false;
}

/** @brief Checks if a continuation line should terminate brief absorption. */
bool ShouldStopBriefContinuation(std::string_view nextTrimmed)
{
    if (nextTrimmed.empty() || nextTrimmed.starts_with("@code") || nextTrimmed.starts_with("\\code") ||
        nextTrimmed.starts_with("@verbatim") || nextTrimmed.starts_with("\\verbatim"))
    {
        return true;
    }
    size_t cmdEnd = 0;
    return IsBlockCommand(nextTrimmed, cmdEnd);
}

/** @brief Extracts an explicit brief command from lines. */
std::string ExtractExplicitBrief(std::vector<std::string>& lines)
{
    size_t explicitBriefIdx = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (IsExplicitBriefStart(TrimLeading(lines[i])))
        {
            explicitBriefIdx = i;
            break;
        }
    }

    if (explicitBriefIdx == std::string::npos)
    {
        return "";
    }

    std::string trimmed = TrimLeading(lines[explicitBriefIdx]);
    std::string briefText = Trim(trimmed.substr(6));
    lines.erase(lines.begin() + explicitBriefIdx);

    while (explicitBriefIdx < lines.size())
    {
        std::string nextTrimmed = TrimLeading(lines[explicitBriefIdx]);
        if (ShouldStopBriefContinuation(nextTrimmed))
        {
            break;
        }
        if (briefText.empty() || (!briefText.ends_with('.') && !briefText.ends_with('!') && !briefText.ends_with('?')))
        {
            if (!briefText.empty())
            {
                briefText += " ";
            }
            briefText += Trim(lines[explicitBriefIdx]);
            lines.erase(lines.begin() + explicitBriefIdx);
        }
        else
        {
            break;
        }
    }

    briefText = RewriteAtInlineCommands(briefText);
    return CleanDescriptionLines(briefText);
}

/** @brief Checks whether a line cannot serve as an implicit brief candidate. */
bool IsIneligibleImplicitBriefLine(std::string_view line)
{
    if (line.starts_with("@code") || line.starts_with("\\code") || line.starts_with("@verbatim") ||
        line.starts_with("\\verbatim") || line.starts_with("-# ") || line.starts_with("-#\t") ||
        line.starts_with("* ") || line.starts_with("- "))
    {
        return true;
    }
    size_t cmdEnd = 0;
    return IsBlockCommand(line, cmdEnd);
}

/** @brief Splits first content line at sentence-ending dot into brief and remainder. */
std::string SplitFirstLineAtDot(std::string& firstLine, size_t dotPos)
{
    size_t cutPos = dotPos + 1;
    while (cutPos < firstLine.size() && IsClosingQuoteOrBracket(firstLine[cutPos]))
    {
        ++cutPos;
    }
    std::string briefText = Trim(firstLine.substr(0, cutPos));
    std::string remainder = TrimLeading(firstLine.substr(cutPos));
    while (!remainder.empty() &&
           (remainder.front() == ';' || std::isspace(static_cast<unsigned char>(remainder.front()))))
    {
        remainder.erase(0, 1);
    }
    bool hasAlnum = std::any_of(remainder.begin(), remainder.end(), [](unsigned char ch) { return std::isalnum(ch); });
    firstLine = hasAlnum ? remainder : "";
    return briefText;
}

/** @brief Extracts an implicit brief from the first content line. */
std::string ExtractImplicitBrief(std::vector<std::string>& lines)
{
    size_t firstContentIdx = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (!Trim(lines[i]).empty())
        {
            firstContentIdx = i;
            break;
        }
    }

    if (firstContentIdx == std::string::npos)
    {
        return "";
    }

    std::string firstLine = TrimLeading(lines[firstContentIdx]);
    if (IsIneligibleImplicitBriefLine(firstLine))
    {
        return "";
    }

    size_t dotPos = FindSentenceEndingDot(firstLine);
    std::string briefText;
    if (dotPos == std::string::npos)
    {
        briefText = Trim(firstLine);
        lines.erase(lines.begin() + firstContentIdx);
    }
    else
    {
        briefText = SplitFirstLineAtDot(firstLine, dotPos);
        if (firstLine.empty())
        {
            lines.erase(lines.begin() + firstContentIdx);
        }
        else
        {
            lines[firstContentIdx] = firstLine;
        }
    }

    briefText = RewriteAtInlineCommands(briefText);
    return CleanDescriptionLines(briefText);
}

/**
 * @brief Extracts the brief description in the pre-pass and removes it from lines.
 */
std::string ExtractBrief(std::vector<std::string>& lines)
{
    std::string brief = ExtractExplicitBrief(lines);
    if (!brief.empty())
    {
        return brief;
    }
    return ExtractImplicitBrief(lines);
}

/** @brief State for comment line segmentation. */
struct SegmenterState
{
    CommentSegment currentSeg;
    bool hasActiveSeg = false;
    bool inCode = false;
    CommentSegment currentCode;
};

/** @brief Flushes any active segment in the segmenter state. */
void FlushActiveSegment(SegmenterState& state, std::vector<CommentSegment>& segments)
{
    if (state.hasActiveSeg && !state.currentSeg.lines.empty())
    {
        segments.push_back(std::move(state.currentSeg));
    }
    state.currentSeg = CommentSegment{};
    state.hasActiveSeg = false;
}

/** @brief Checks if a line starts a verbatim code block and determines its command length. */
bool IsVerbatimCodeStart(std::string_view line, size_t& cmdLen, bool& isVerbatim)
{
    if (line.starts_with("@verbatim") || line.starts_with("\\verbatim"))
    {
        cmdLen = 9;
        isVerbatim = true;
        return true;
    }
    if (line.starts_with("@code") || line.starts_with("\\code"))
    {
        cmdLen = 5;
        isVerbatim = false;
        return true;
    }
    return false;
}

/** @brief Checks if a line ends a verbatim code block. */
bool IsVerbatimCodeEnd(std::string_view line)
{
    std::string_view trimmed = line;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
    {
        trimmed.remove_prefix(1);
    }
    return trimmed.starts_with("@endcode") || trimmed.starts_with("\\endcode") || trimmed.starts_with("@endverbatim") ||
           trimmed.starts_with("\\endverbatim");
}

/** @brief Parses language identifier from code block command arguments. */
std::string ParseCodeLanguage(std::string_view afterCmd)
{
    std::string lang = Trim(std::string(afterCmd));
    if (lang.starts_with('{') && lang.ends_with('}'))
    {
        lang = lang.substr(1, lang.size() - 2);
    }
    lang = Trim(lang);
    if (lang.starts_with('.'))
    {
        lang = lang.substr(1);
    }
    return Trim(lang);
}

/** @brief Processes a line when parser is inside a verbatim code block. Returns true if handled. */
bool TryProcessCodeLine(const std::string& line, SegmenterState& state, std::vector<CommentSegment>& segments)
{
    if (!state.inCode)
    {
        return false;
    }
    if (IsVerbatimCodeEnd(line))
    {
        state.inCode = false;
        segments.push_back(std::move(state.currentCode));
        state.currentCode = CommentSegment{};
        state.currentCode.kind = CommentSegmentKind::VerbatimCode;
        return true;
    }
    state.currentCode.lines.push_back(line);
    return true;
}

/** @brief Checks and initiates a verbatim code block segment if line matches code start syntax. */
bool TryStartCodeBlock(std::string_view trimmedLeading, SegmenterState& state, std::vector<CommentSegment>& segments)
{
    size_t cmdLen = 0;
    bool isVerbatim = false;
    if (!IsVerbatimCodeStart(trimmedLeading, cmdLen, isVerbatim))
    {
        return false;
    }
    std::string_view afterCmd = trimmedLeading.substr(cmdLen);
    if (!afterCmd.empty() && !std::isspace(static_cast<unsigned char>(afterCmd.front())) && afterCmd.front() != '{' &&
        afterCmd.front() != '.')
    {
        return false;
    }

    FlushActiveSegment(state, segments);
    state.inCode = true;
    state.currentCode = CommentSegment{};
    state.currentCode.kind = CommentSegmentKind::VerbatimCode;
    if (!isVerbatim)
    {
        state.currentCode.language = ParseCodeLanguage(afterCmd);
    }
    return true;
}

/** @brief Appends content line to active tag or text segment. */
void AppendTextOrTagLine(const std::string& line, SegmenterState& state, std::vector<CommentSegment>& segments)
{
    if (state.hasActiveSeg && state.currentSeg.kind == CommentSegmentKind::Tag)
    {
        state.currentSeg.lines.push_back(line);
    }
    else
    {
        if (!state.hasActiveSeg || state.currentSeg.kind != CommentSegmentKind::Text)
        {
            FlushActiveSegment(state, segments);
            state.currentSeg.kind = CommentSegmentKind::Text;
            state.hasActiveSeg = true;
        }
        state.currentSeg.lines.push_back(line);
    }
}

/** @brief Processes a single line inside the segmenter loop. */
void ProcessSegmentLine(const std::string& line, SegmenterState& state, std::vector<CommentSegment>& segments)
{
    if (TryProcessCodeLine(line, state, segments))
    {
        return;
    }

    std::string trimmedLeading = TrimLeading(line);
    if (TryStartCodeBlock(trimmedLeading, state, segments))
    {
        return;
    }

    if (Trim(line).empty())
    {
        FlushActiveSegment(state, segments);
        return;
    }

    size_t cmdEnd = 0;
    if (IsBlockCommand(trimmedLeading, cmdEnd))
    {
        FlushActiveSegment(state, segments);
        state.currentSeg.kind = CommentSegmentKind::Tag;
        state.hasActiveSeg = true;

        std::string normLine = line;
        size_t firstNonSpace = normLine.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos && normLine[firstNonSpace] == '\\')
        {
            normLine[firstNonSpace] = '@';
        }
        state.currentSeg.lines.push_back(std::move(normLine));
        return;
    }

    AppendTextOrTagLine(line, state, segments);
}

/**
 * @brief Segments comment lines into Tag, Text, and VerbatimCode blocks.
 */
std::vector<CommentSegment> SegmentCommentLines(const std::vector<std::string>& lines)
{
    std::vector<CommentSegment> segments;
    SegmenterState state;
    state.currentCode.kind = CommentSegmentKind::VerbatimCode;

    for (const auto& line : lines)
    {
        ProcessSegmentLine(line, state, segments);
    }

    if (state.inCode)
    {
        segments.push_back(std::move(state.currentCode));
    }
    else
    {
        FlushActiveSegment(state, segments);
    }

    return segments;
}

/** @brief Wraps lines into a synthetic block comment for parsing. */
std::string WrapInSyntheticComment(const std::vector<std::string>& lines)
{
    std::string synthetic = "/**\n";
    for (const auto& l : lines)
    {
        if (l.empty())
        {
            synthetic += " *\n";
        }
        else
        {
            synthetic += " * " + l + "\n";
        }
    }
    synthetic += " */";
    return synthetic;
}

/** @brief Helper to find a child node with a matching type name. */
TSNode FindChildByType(TSNode node, const char* typeName)
{
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (std::strcmp(ts_node_type(child), typeName) == 0)
        {
            return child;
        }
    }
    return TSNode{};
}

/** @brief Renders a code_word inline node to Markdown. */
std::string RenderCodeWordNode(TSNode node, const std::string& sourceCode)
{
    TSNode codeChild = FindChildByType(node, "code");
    if (!ts_node_is_null(codeChild))
    {
        return "`" + std::string(parser::DoxygenParser::GetNodeText(codeChild, sourceCode)) + "`";
    }
    std::string_view full = parser::DoxygenParser::GetNodeText(node, sourceCode);
    if (full.starts_with("\\c"))
    {
        full.remove_prefix(2);
    }
    while (!full.empty() && (full.front() == ' ' || full.front() == '\t'))
    {
        full.remove_prefix(1);
    }
    return "`" + std::string(full) + "`";
}

/** @brief Renders an emphasis inline node to Markdown. */
std::string RenderEmphasisNode(TSNode node, const std::string& sourceCode)
{
    TSNode textChild = FindChildByType(node, "text");
    if (!ts_node_is_null(textChild))
    {
        return "*" + std::string(parser::DoxygenParser::GetNodeText(textChild, sourceCode)) + "*";
    }
    std::string_view full = parser::DoxygenParser::GetNodeText(node, sourceCode);
    if (full.starts_with("\\a"))
    {
        full.remove_prefix(2);
    }
    while (!full.empty() && (full.front() == ' ' || full.front() == '\t'))
    {
        full.remove_prefix(1);
    }
    return "*" + std::string(full) + "*";
}

/** @brief Renders a link inline node to Markdown. */
std::string RenderLinkNode(TSNode node, const std::string& sourceCode)
{
    std::string nodeText = std::string(parser::DoxygenParser::GetNodeText(node, sourceCode));
    std::string linkText;
    TSNode textChild = FindChildByType(node, "text");
    if (!ts_node_is_null(textChild))
    {
        linkText = std::string(parser::DoxygenParser::GetNodeText(textChild, sourceCode));
    }

    std::string url;
    size_t hrefPos = nodeText.find("href=");
    if (hrefPos != std::string::npos)
    {
        size_t quoteStart = hrefPos + 5;
        if (quoteStart < nodeText.size() && (nodeText[quoteStart] == '"' || nodeText[quoteStart] == '\''))
        {
            char q = nodeText[quoteStart];
            size_t quoteEnd = nodeText.find(q, quoteStart + 1);
            if (quoteEnd != std::string::npos)
            {
                url = nodeText.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            }
        }
    }
    if (!url.empty())
    {
        return !linkText.empty() ? "[" + linkText + "](" + url + ")" : "[" + url + "](" + url + ")";
    }
    return linkText.empty() ? nodeText : linkText;
}

/**
 * @brief Renders an inline syntax node (code_word, emphasis, link, function_link) to Markdown.
 */
std::string RenderInlineNode(TSNode node, const std::string& sourceCode)
{
    const char* type = ts_node_type(node);
    if (std::strcmp(type, "code_word") == 0)
    {
        return RenderCodeWordNode(node, sourceCode);
    }
    if (std::strcmp(type, "emphasis") == 0)
    {
        return RenderEmphasisNode(node, sourceCode);
    }
    if (std::strcmp(type, "function_link") == 0)
    {
        return "`" + std::string(parser::DoxygenParser::GetNodeText(node, sourceCode)) + "`";
    }
    if (std::strcmp(type, "link") == 0)
    {
        return RenderLinkNode(node, sourceCode);
    }

    return std::string(parser::DoxygenParser::GetNodeText(node, sourceCode));
}

/**
 * @brief Renders a description node by walking its byte range and substituting named children.
 */
std::string RenderDescription(TSNode descNode, const std::string& sourceCode)
{
    uint32_t startByte = ts_node_start_byte(descNode);
    uint32_t endByte = ts_node_end_byte(descNode);
    if (startByte >= endByte || endByte > sourceCode.size())
    {
        return "";
    }

    std::string raw;
    uint32_t cursor = startByte;
    uint32_t childCount = ts_node_named_child_count(descNode);

    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode child = ts_node_named_child(descNode, i);
        uint32_t childStart = ts_node_start_byte(child);
        uint32_t childEnd = ts_node_end_byte(child);

        if (childStart > cursor)
        {
            std::string verbatim = sourceCode.substr(cursor, childStart - cursor);
            raw.append(RewriteAtInlineCommands(verbatim));
        }

        raw.append(RenderInlineNode(child, sourceCode));
        cursor = std::max(cursor, childEnd);
    }

    if (cursor < endByte)
    {
        std::string verbatim = sourceCode.substr(cursor, endByte - cursor);
        raw.append(RewriteAtInlineCommands(verbatim));
    }

    return CleanDescriptionLines(raw);
}

/**
 * @brief Extracts the brief text from a brief_header node.
 */
std::string ProcessBriefHeader(TSNode briefNode, const std::string& sourceCode)
{
    TSNode descChild = FindChildByType(briefNode, "brief_description");
    if (!ts_node_is_null(descChild))
    {
        return RenderDescription(descChild, sourceCode);
    }

    TSNode tagChild = FindChildByType(briefNode, "tag_name");
    uint32_t start = !ts_node_is_null(tagChild) ? ts_node_end_byte(tagChild) : ts_node_start_byte(briefNode);
    uint32_t end = ts_node_end_byte(briefNode);
    if (end > start && end <= sourceCode.size())
    {
        return CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
    }

    return "";
}

/**
 * @brief Formats a code_block node into a Markdown fenced block.
 */
std::string ProcessCodeBlock(TSNode blockNode, const std::string& sourceCode)
{
    TSNode langNode = FindChildByType(blockNode, "code_block_language");
    std::string lang;
    if (!ts_node_is_null(langNode))
    {
        lang = ParseCodeLanguage(parser::DoxygenParser::GetNodeText(langNode, sourceCode));
    }

    TSNode contentNode = FindChildByType(blockNode, "code_block_content");
    std::string content;
    if (!ts_node_is_null(contentNode))
    {
        content = std::string(parser::DoxygenParser::GetNodeText(contentNode, sourceCode));
    }

    auto lines = SplitLines(content);
    for (auto& line : lines)
    {
        line = StripLineLeadingStar(line);
    }

    while (!lines.empty() && Trim(lines.front()).empty())
    {
        lines.erase(lines.begin());
    }
    while (!lines.empty() && Trim(lines.back()).empty())
    {
        lines.pop_back();
    }

    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i > 0)
        {
            joined += "\n";
        }
        joined += lines[i];
    }

    return "```" + lang + "\n" + joined + "\n```";
}

struct ParamItem
{
    std::vector<std::string> names;
    std::string direction; // "in", "out", "in,out", or empty
    std::string description;
};

struct TParamItem
{
    std::string name;
    std::string description;
};

struct RetValItem
{
    std::string value;
    std::string description;
};

enum class DocBlockKind
{
    Brief,
    Body,
    CodeBlock,
    TParam,
    Param,
    Return,
    RetVal,
    Admonition
};

struct DocBlock
{
    DocBlockKind kind;
    std::string text;
    std::string label;
    TParamItem tparam;
    ParamItem param;
    RetValItem retval;

    static DocBlock MakeBrief(std::string t)
    {
        DocBlock b;
        b.kind = DocBlockKind::Brief;
        b.text = std::move(t);
        return b;
    }

    static DocBlock MakeBody(std::string t)
    {
        DocBlock b;
        b.kind = DocBlockKind::Body;
        b.text = std::move(t);
        return b;
    }

    static DocBlock MakeCodeBlock(std::string t)
    {
        DocBlock b;
        b.kind = DocBlockKind::CodeBlock;
        b.text = std::move(t);
        return b;
    }

    static DocBlock MakeTParam(TParamItem tp)
    {
        DocBlock b;
        b.kind = DocBlockKind::TParam;
        b.tparam = std::move(tp);
        return b;
    }

    static DocBlock MakeParam(ParamItem p)
    {
        DocBlock b;
        b.kind = DocBlockKind::Param;
        b.param = std::move(p);
        return b;
    }

    static DocBlock MakeReturn(std::string t)
    {
        DocBlock b;
        b.kind = DocBlockKind::Return;
        b.text = std::move(t);
        return b;
    }

    static DocBlock MakeRetVal(RetValItem r)
    {
        DocBlock b;
        b.kind = DocBlockKind::RetVal;
        b.retval = std::move(r);
        return b;
    }

    static DocBlock MakeAdmonition(std::string l, std::string t)
    {
        DocBlock b;
        b.kind = DocBlockKind::Admonition;
        b.label = std::move(l);
        b.text = std::move(t);
        return b;
    }
};

/** @brief Normalizes storageclass bracketed parameter direction text. */
std::string NormalizeParamDirection(std::string_view rawSc)
{
    size_t open = rawSc.find('[');
    size_t close = rawSc.rfind(']');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open)
    {
        return "";
    }

    std::string inner = Trim(std::string(rawSc.substr(open + 1, close - open - 1)));
    std::string lowerInner = ToLower(inner);
    std::string compact;
    for (char c : lowerInner)
    {
        if (c != ' ' && c != '\t')
        {
            compact += c;
        }
    }
    if (compact == "inout" || compact == "in,out")
    {
        return "in,out";
    }
    if (compact == "in" || compact == "out")
    {
        return compact;
    }
    return compact;
}

/** @brief Extracts parameter direction, names, and description from tag children. */
void ProcessParamTagChildren(TSNode tagNode, const std::string& sourceCode, ParamItem& item)
{
    uint32_t childCount = ts_node_child_count(tagNode);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode child = ts_node_child(tagNode, i);
        const char* type = ts_node_type(child);
        if (std::strcmp(type, "storageclass") == 0)
        {
            item.direction = NormalizeParamDirection(parser::DoxygenParser::GetNodeText(child, sourceCode));
        }
        else if (std::strcmp(type, "identifier") == 0 || std::strcmp(type, "qualified_identifier") == 0)
        {
            std::string name = Trim(std::string(parser::DoxygenParser::GetNodeText(child, sourceCode)));
            if (!name.empty())
            {
                item.names.push_back(std::move(name));
            }
        }
        else if (std::strcmp(type, "description") == 0)
        {
            item.description = RenderDescription(child, sourceCode);
        }
    }
}

/**
 * @brief Extracts parameter names, optional direction, and description from a @param tag.
 */
ParamItem ProcessParamTag(TSNode tagNode, const std::string& sourceCode)
{
    ParamItem item;
    ProcessParamTagChildren(tagNode, sourceCode, item);

    if (item.names.empty() && !item.description.empty())
    {
        size_t sp = item.description.find_first_of(" \t");
        if (sp != std::string::npos)
        {
            item.names.push_back(item.description.substr(0, sp));
            item.description = Trim(item.description.substr(sp + 1));
        }
        else
        {
            item.names.push_back(item.description);
            item.description = "";
        }
    }

    return item;
}

/**
 * @brief Splits the template parameter name from the description for a @tparam tag.
 */
TParamItem ProcessTParamTag(TSNode tagNode, const std::string& sourceCode)
{
    TParamItem item;
    TSNode descChild = FindChildByType(tagNode, "description");
    std::string desc;
    if (!ts_node_is_null(descChild))
    {
        desc = RenderDescription(descChild, sourceCode);
    }
    else
    {
        TSNode nameChild = FindChildByType(tagNode, "tag_name");
        uint32_t start = !ts_node_is_null(nameChild) ? ts_node_end_byte(nameChild) : ts_node_start_byte(tagNode);
        uint32_t end = ts_node_end_byte(tagNode);
        if (end > start && end <= sourceCode.size())
        {
            desc = CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
        }
    }

    desc = Trim(desc);
    size_t sp = desc.find_first_of(" \t");
    if (sp != std::string::npos)
    {
        item.name = desc.substr(0, sp);
        item.description = Trim(desc.substr(sp + 1));
    }
    else
    {
        item.name = desc;
        item.description = "";
    }
    return item;
}

/** @brief Extracts the description string for return tags. */
std::string ProcessReturnTag(TSNode tagNode, const std::string& sourceCode)
{
    TSNode descChild = FindChildByType(tagNode, "description");
    if (!ts_node_is_null(descChild))
    {
        return RenderDescription(descChild, sourceCode);
    }
    return "";
}

/** @brief Extracts description text from an admonition tag node. */
std::string ExtractAdmonitionDescription(TSNode tagNode, const std::string& sourceCode)
{
    TSNode descChild = FindChildByType(tagNode, "description");
    if (!ts_node_is_null(descChild))
    {
        return RenderDescription(descChild, sourceCode);
    }

    std::vector<std::string> parts;
    uint32_t count = ts_node_child_count(tagNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode c = ts_node_child(tagNode, i);
        const char* ct = ts_node_type(c);
        if (std::strcmp(ct, "identifier") == 0 || std::strcmp(ct, "function_link") == 0 ||
            std::strcmp(ct, "function") == 0)
        {
            parts.push_back(std::string(parser::DoxygenParser::GetNodeText(c, sourceCode)));
        }
    }
    if (!parts.empty())
    {
        std::string desc;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
            {
                desc += ", ";
            }
            desc += parts[i];
        }
        return desc;
    }

    TSNode nameChild = FindChildByType(tagNode, "tag_name");
    uint32_t start = !ts_node_is_null(nameChild) ? ts_node_end_byte(nameChild) : ts_node_start_byte(tagNode);
    uint32_t end = ts_node_end_byte(tagNode);
    if (end > start && end <= sourceCode.size())
    {
        return CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
    }
    return "";
}

/** @brief Maps a cleaned Doxygen admonition tag name to its canonical Markdown label. */
std::string MapAdmonitionLabel(std::string_view cleanedTag)
{
    static constexpr std::pair<std::string_view, std::string_view> kKnownLabels[] = {
        {"note", "Note"},
        {"warning", "Warning"},
        {"attention", "Attention"},
        {"caution", "Caution"},
        {"deprecated", "Deprecated"},
        {"see", "See also"},
        {"sa", "See also"},
        {"remark", "Remark"},
        {"remarks", "Remark"},
        {"since", "Since"},
        {"todo", "Todo"},
        {"bug", "Bug"},
        {"pre", "Pre"},
        {"post", "Post"},
        {"throw", "Throws"},
        {"throws", "Throws"},
        {"exception", "Throws"},
    };

    std::string lower = ToLower(std::string(cleanedTag));
    for (const auto& [tag, label] : kKnownLabels)
    {
        if (lower == tag)
        {
            return std::string(label);
        }
    }

    char first = static_cast<char>(std::toupper(static_cast<unsigned char>(cleanedTag[0])));
    return first + std::string(cleanedTag.substr(1));
}

/**
 * @brief Maps tag name and description to an Admonition block.
 */
DocBlock ProcessAdmonitionTag(const std::string& cleanedTag, TSNode tagNode, const std::string& sourceCode)
{
    std::string desc = Trim(ExtractAdmonitionDescription(tagNode, sourceCode));
    std::string label = MapAdmonitionLabel(cleanedTag);
    return DocBlock::MakeAdmonition(std::move(label), std::move(desc));
}

/**
 * @brief Extracts value and description from a @retval tag.
 */
RetValItem ProcessRetValTag(TSNode tagNode, const std::string& sourceCode)
{
    RetValItem item;
    TSNode descChild = FindChildByType(tagNode, "description");
    std::string desc;
    if (!ts_node_is_null(descChild))
    {
        desc = RenderDescription(descChild, sourceCode);
    }
    else
    {
        TSNode nameChild = FindChildByType(tagNode, "tag_name");
        uint32_t start = !ts_node_is_null(nameChild) ? ts_node_end_byte(nameChild) : ts_node_start_byte(tagNode);
        uint32_t end = ts_node_end_byte(tagNode);
        if (end > start && end <= sourceCode.size())
        {
            desc = CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
        }
    }

    desc = Trim(desc);
    size_t sp = desc.find_first_of(" \t");
    if (sp != std::string::npos)
    {
        item.value = desc.substr(0, sp);
        item.description = Trim(desc.substr(sp + 1));
    }
    else
    {
        item.value = desc;
        item.description = "";
    }
    return item;
}

/**
 * @brief Checks whether a Doxygen tag is a structural declaration command.
 */
bool IsStructuralTag(std::string_view tag)
{
    static const char* const kStructuralTags[] = {
        "class",     "struct",     "union",     "enum",     "typedef",  "typealias",  "interface",
        "fn",        "function",   "method",    "property", "var",      "variable",   "overload",
        "file",      "headerfile", "namespace", "package",  "defgroup", "addtogroup", "ingroup",
        "weakgroup", "endgroup",   "name",      "def",      "define"};
    for (const char* st : kStructuralTags)
    {
        if (tag == st)
        {
            return true;
        }
    }
    return false;
}

/** @brief Extracts documentation lines from multi-line structural tags. */
std::string ExtractMultiLineStructuralDescription(const std::vector<std::string>& lines)
{
    std::string subDesc;
    for (size_t i = 1; i < lines.size(); ++i)
    {
        std::string l = Trim(lines[i]);
        if (!l.empty())
        {
            if (!subDesc.empty())
            {
                subDesc += " ";
            }
            subDesc += l;
        }
    }
    return CleanDescriptionLines(subDesc);
}

/** @brief Extracts descriptive text after single-line structural entity identifier. */
std::string ExtractSingleLineStructuralDescription(std::string_view raw)
{
    size_t sp = raw.find_first_of(" \t");
    if (sp != std::string_view::npos)
    {
        std::string remainder = Trim(std::string(raw.substr(sp + 1)));
        if (!remainder.empty() && !remainder.ends_with(')') && !remainder.ends_with(';'))
        {
            return remainder;
        }
    }
    return "";
}

/**
 * @brief Extracts attached body description following a structural tag, if any.
 */
std::string ProcessStructuralTagDescription(TSNode tagNode, const std::string& sourceCode)
{
    TSNode descChild = FindChildByType(tagNode, "description");
    std::string raw;
    if (!ts_node_is_null(descChild))
    {
        raw = RenderDescription(descChild, sourceCode);
    }
    else
    {
        TSNode nameChild = FindChildByType(tagNode, "tag_name");
        uint32_t start = !ts_node_is_null(nameChild) ? ts_node_end_byte(nameChild) : ts_node_start_byte(tagNode);
        uint32_t end = ts_node_end_byte(tagNode);
        if (end > start && end <= sourceCode.size())
        {
            raw = CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
        }
    }

    raw = Trim(raw);
    if (raw.empty())
    {
        return "";
    }

    TSNode typeChild = FindChildByType(tagNode, "type");
    if (!ts_node_is_null(typeChild))
    {
        return !ts_node_is_null(descChild) ? raw : "";
    }

    auto lines = SplitLines(raw);
    if (lines.size() > 1)
    {
        return ExtractMultiLineStructuralDescription(lines);
    }

    return ExtractSingleLineStructuralDescription(raw);
}

/** @brief Appends text to a string block with proper spacing. */
void AppendToBlockText(std::string& target, const std::string& addition)
{
    if (addition.empty())
    {
        return;
    }
    if (target.empty())
    {
        target = addition;
        return;
    }
    if (!target.ends_with(' ') && !target.ends_with('\t') && !target.ends_with('(') && !target.ends_with('[') &&
        !target.ends_with('{'))
    {
        target += ' ';
    }
    target += addition;
}

/** @brief Appends text to a DocBlock target. */
void AppendToDocBlock(DocBlock& block, const std::string& text)
{
    switch (block.kind)
    {
    case DocBlockKind::Brief:
    case DocBlockKind::Body:
    case DocBlockKind::Return:
    case DocBlockKind::Admonition:
        AppendToBlockText(block.text, text);
        break;
    case DocBlockKind::TParam:
        AppendToBlockText(block.tparam.description, text);
        break;
    case DocBlockKind::Param:
        AppendToBlockText(block.param.description, text);
        break;
    case DocBlockKind::RetVal:
        AppendToBlockText(block.retval.description, text);
        break;
    case DocBlockKind::CodeBlock:
        break;
    }
}

/** @brief Splits an inline tag description into target word and trailing remainder. */
std::pair<std::string, std::string> SplitInlineTagWord(std::string_view desc)
{
    std::string trimmedDesc = TrimLeading(std::string(desc));
    std::string X;
    std::string remainder;
    size_t sp = trimmedDesc.find_first_of(" \t");
    if (sp != std::string::npos)
    {
        X = trimmedDesc.substr(0, sp);
        remainder = trimmedDesc.substr(sp);
    }
    else
    {
        X = trimmedDesc;
    }

    size_t punct = X.size();
    while (punct > 0 && IsTrailingPunctuation(X[punct - 1]))
    {
        --punct;
    }
    if (punct > 0 && punct < X.size())
    {
        remainder = X.substr(punct) + remainder;
        X = X.substr(0, punct);
    }
    return {std::move(X), std::move(remainder)};
}

/** @brief Formats an inline word into Markdown depending on tag command. */
std::string FormatInlineWord(std::string_view cmd, std::string_view word)
{
    if (cmd == "c" || cmd == "p" || cmd == "ref")
    {
        return "`" + std::string(word) + "`";
    }
    if (cmd == "b")
    {
        return "**" + std::string(word) + "**";
    }
    if (cmd == "a" || cmd == "e" || cmd == "em")
    {
        return "*" + std::string(word) + "*";
    }
    return std::string(word);
}

/**
 * @brief Merges an inline formatting tag (\b, \e, \em, \a, \c, \p, \ref) into the preceding block.
 */
void MergeInlineTag(const std::string& cmd, TSNode tagNode, const std::string& sourceCode,
                    std::vector<DocBlock>& blocks)
{
    TSNode descChild = FindChildByType(tagNode, "description");
    std::string desc;
    if (!ts_node_is_null(descChild))
    {
        desc = RenderDescription(descChild, sourceCode);
    }
    else
    {
        TSNode nameChild = FindChildByType(tagNode, "tag_name");
        uint32_t start = !ts_node_is_null(nameChild) ? ts_node_end_byte(nameChild) : ts_node_start_byte(tagNode);
        uint32_t end = ts_node_end_byte(tagNode);
        if (end > start && end <= sourceCode.size())
        {
            desc = CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
        }
    }

    auto [word, remainder] = SplitInlineTagWord(desc);
    std::string inlineRendered = FormatInlineWord(cmd, word) + remainder;

    if (blocks.empty() || blocks.back().kind == DocBlockKind::CodeBlock)
    {
        blocks.push_back(DocBlock::MakeBody(std::move(inlineRendered)));
    }
    else
    {
        AppendToDocBlock(blocks.back(), inlineRendered);
    }
}

/** @brief Extracts raw tag name from a tag AST node. */
std::string ExtractRawTagName(TSNode child, const std::string& syntheticDoc)
{
    TSNode nameNode = FindChildByType(child, "tag_name");
    std::string rawTagName;
    if (!ts_node_is_null(nameNode))
    {
        rawTagName = std::string(parser::DoxygenParser::GetNodeText(nameNode, syntheticDoc));
    }
    else
    {
        TSNode scNode = FindChildByType(child, "storageclass");
        if (!ts_node_is_null(scNode))
        {
            rawTagName = "param";
        }
    }
    if (rawTagName.starts_with('@') || rawTagName.starts_with('\\'))
    {
        rawTagName = rawTagName.substr(1);
    }
    return rawTagName;
}

/** @brief Tries to dispatch common inline, parameter, return, or retval tags. Returns true if handled. */
bool TryDispatchCommonTag(std::string_view lowerTag, TSNode child, const std::string& syntheticDoc,
                          std::vector<DocBlock>& blocks)
{
    if (lowerTag == "b" || lowerTag == "e" || lowerTag == "em" || lowerTag == "a" || lowerTag == "c" ||
        lowerTag == "p" || lowerTag == "ref")
    {
        MergeInlineTag(std::string(lowerTag), child, syntheticDoc, blocks);
        return true;
    }
    if (lowerTag == "param")
    {
        blocks.push_back(DocBlock::MakeParam(ProcessParamTag(child, syntheticDoc)));
        return true;
    }
    if (lowerTag == "tparam")
    {
        blocks.push_back(DocBlock::MakeTParam(ProcessTParamTag(child, syntheticDoc)));
        return true;
    }
    if (lowerTag == "return" || lowerTag == "returns" || lowerTag == "result")
    {
        blocks.push_back(DocBlock::MakeReturn(ProcessReturnTag(child, syntheticDoc)));
        return true;
    }
    if (lowerTag == "retval")
    {
        blocks.push_back(DocBlock::MakeRetVal(ProcessRetValTag(child, syntheticDoc)));
        return true;
    }
    return false;
}

/** @brief Dispatches tag AST nodes to corresponding block builders. */
void DispatchSegmentTagNode(TSNode child, const std::string& syntheticDoc, std::vector<DocBlock>& blocks)
{
    std::string rawTagName = ExtractRawTagName(child, syntheticDoc);
    std::string lowerTag = ToLower(rawTagName);

    if (TryDispatchCommonTag(lowerTag, child, syntheticDoc, blocks))
    {
        return;
    }
    if (lowerTag == "brief" || lowerTag == "details")
    {
        std::string brief = ProcessReturnTag(child, syntheticDoc);
        if (!brief.empty())
        {
            blocks.push_back(DocBlock::MakeBody(std::move(brief)));
        }
    }
    else if (IsStructuralTag(lowerTag))
    {
        std::string desc = ProcessStructuralTagDescription(child, syntheticDoc);
        if (!desc.empty())
        {
            blocks.push_back(DocBlock::MakeBody(std::move(desc)));
        }
    }
    else if (!rawTagName.empty())
    {
        blocks.push_back(ProcessAdmonitionTag(rawTagName, child, syntheticDoc));
    }
}

/** @brief Dispatches a child AST node of a parsed segment. */
void DispatchSegmentChildNode(TSNode child, const std::string& syntheticDoc, std::vector<DocBlock>& blocks)
{
    const char* type = ts_node_type(child);
    if (std::strcmp(type, "brief_header") == 0)
    {
        std::string brief = ProcessBriefHeader(child, syntheticDoc);
        if (!brief.empty())
        {
            blocks.push_back(DocBlock::MakeBody(std::move(brief)));
        }
    }
    else if (std::strcmp(type, "description") == 0)
    {
        std::string body = RenderDescription(child, syntheticDoc);
        if (!body.empty())
        {
            blocks.push_back(DocBlock::MakeBody(std::move(body)));
        }
    }
    else if (std::strcmp(type, "code_block") == 0)
    {
        std::string code = ProcessCodeBlock(child, syntheticDoc);
        if (!code.empty())
        {
            blocks.push_back(DocBlock::MakeCodeBlock(std::move(code)));
        }
    }
    else if (std::strcmp(type, "tag") == 0)
    {
        DispatchSegmentTagNode(child, syntheticDoc, blocks);
    }
    else if (std::strcmp(type, "_text_line") == 0)
    {
        std::string line = std::string(parser::DoxygenParser::GetNodeText(child, syntheticDoc));
        line = CleanDescriptionLines(RewriteAtInlineCommands(line));
        if (!line.empty())
        {
            if (!blocks.empty() && blocks.back().kind == DocBlockKind::Body)
            {
                AppendToBlockText(blocks.back().text, line);
            }
            else
            {
                blocks.push_back(DocBlock::MakeBody(std::move(line)));
            }
        }
    }
}

/**
 * @brief Parses a single synthetic document segment and collects AST DocBlocks.
 */
void ParseDocSegment(const std::string& syntheticDoc, std::vector<DocBlock>& blocks)
{
    if (Trim(syntheticDoc).empty())
    {
        return;
    }

    parser::DoxygenParser parser;
    TSTree* tree = parser.Parse(syntheticDoc);
    if (!tree)
    {
        return;
    }

    TSNode root = ts_tree_root_node(tree);
    if (ts_node_is_null(root))
    {
        ts_tree_delete(tree);
        return;
    }

    uint32_t childCount = ts_node_child_count(root);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        DispatchSegmentChildNode(ts_node_child(root, i), syntheticDoc, blocks);
    }

    ts_tree_delete(tree);
}

/** @brief Assembles brief and general body paragraphs/code blocks into output sections. */
void AssembleBriefAndBody(std::string briefText, const std::vector<DocBlock>& blocks,
                          std::vector<std::string>& sections)
{
    size_t firstBriefBlockIdx = std::string::npos;
    if (!briefText.empty())
    {
        sections.push_back(std::move(briefText));
    }
    else
    {
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            if (blocks[i].kind == DocBlockKind::Brief && !blocks[i].text.empty())
            {
                sections.push_back(blocks[i].text);
                firstBriefBlockIdx = i;
                break;
            }
        }
    }

    for (size_t i = 0; i < blocks.size(); ++i)
    {
        if (i == firstBriefBlockIdx)
        {
            continue;
        }
        const auto& b = blocks[i];
        if ((b.kind == DocBlockKind::Body || b.kind == DocBlockKind::Brief) && !b.text.empty())
        {
            sections.push_back(b.text);
        }
        else if (b.kind == DocBlockKind::CodeBlock && !b.text.empty())
        {
            sections.push_back(b.text);
        }
    }
}

/** @brief Assembles template parameter bullets into output sections. */
void AssembleTParamBullets(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
{
    std::vector<std::string> tparamBullets;
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::TParam)
        {
            std::string bullet = "* `" + b.tparam.name + "`";
            if (!b.tparam.description.empty())
            {
                bullet += ": " + b.tparam.description;
            }
            tparamBullets.push_back(std::move(bullet));
        }
    }
    if (!tparamBullets.empty())
    {
        std::string joined;
        for (size_t i = 0; i < tparamBullets.size(); ++i)
        {
            if (i > 0)
            {
                joined += "\n";
            }
            joined += tparamBullets[i];
        }
        sections.push_back(std::move(joined));
    }
}

/** @brief Assembles parameter bullets into output sections. */
void AssembleParamBullets(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
{
    std::vector<std::string> paramBullets;
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::Param)
        {
            for (const auto& name : b.param.names)
            {
                std::string bullet = "* `" + name + "`";
                if (!b.param.direction.empty())
                {
                    bullet += " *(" + b.param.direction + ")*";
                }
                if (!b.param.description.empty())
                {
                    bullet += ": " + b.param.description;
                }
                paramBullets.push_back(std::move(bullet));
            }
        }
    }
    if (!paramBullets.empty())
    {
        std::string joined;
        for (size_t i = 0; i < paramBullets.size(); ++i)
        {
            if (i > 0)
            {
                joined += "\n";
            }
            joined += paramBullets[i];
        }
        sections.push_back(std::move(joined));
    }
}

/** @brief Assembles return and retval sections into output sections. */
void AssembleReturnSection(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
{
    std::string returnSection;
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::Return)
        {
            returnSection = "**Returns:**";
            if (!b.text.empty())
            {
                returnSection += " " + b.text;
            }
            break;
        }
    }

    std::vector<std::string> retvalBullets;
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::RetVal)
        {
            std::string bullet = "* `" + b.retval.value + "`";
            if (!b.retval.description.empty())
            {
                bullet += ": " + b.retval.description;
            }
            retvalBullets.push_back(std::move(bullet));
        }
    }
    if (!retvalBullets.empty())
    {
        if (returnSection.empty())
        {
            returnSection = "**Returns:**";
        }
        for (const auto& bullet : retvalBullets)
        {
            returnSection += "\n" + bullet;
        }
    }
    if (!returnSection.empty())
    {
        sections.push_back(std::move(returnSection));
    }
}

/** @brief Assembles admonitions in source order into output sections. */
void AssembleAdmonitions(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
{
    for (const auto& b : blocks)
    {
        if (b.kind == DocBlockKind::Admonition)
        {
            std::string adm = "> **" + b.label + ":**";
            if (!b.text.empty())
            {
                adm += " " + b.text;
            }
            sections.push_back(std::move(adm));
        }
    }
}

/**
 * @brief Assembles extracted blocks and brief into canonical clangd Markdown format.
 */
std::string AssembleMarkdown(std::string briefText, const std::vector<DocBlock>& blocks)
{
    std::vector<std::string> outputSections;
    AssembleBriefAndBody(std::move(briefText), blocks, outputSections);
    AssembleTParamBullets(blocks, outputSections);
    AssembleParamBullets(blocks, outputSections);
    AssembleReturnSection(blocks, outputSections);
    AssembleAdmonitions(blocks, outputSections);

    std::string finalMarkdown;
    for (size_t i = 0; i < outputSections.size(); ++i)
    {
        if (i > 0)
        {
            finalMarkdown += "\n\n";
        }
        finalMarkdown += outputSections[i];
    }

    return Trim(finalMarkdown);
}

/** @brief Processes a verbatim code comment segment and appends a code block. */
void ProcessVerbatimCodeSegment(CommentSegment& seg, std::vector<DocBlock>& allBlocks)
{
    while (!seg.lines.empty() && Trim(seg.lines.front()).empty())
    {
        seg.lines.erase(seg.lines.begin());
    }
    while (!seg.lines.empty() && Trim(seg.lines.back()).empty())
    {
        seg.lines.pop_back();
    }

    std::string joined;
    for (size_t i = 0; i < seg.lines.size(); ++i)
    {
        if (i > 0)
        {
            joined += "\n";
        }
        joined += seg.lines[i];
    }

    std::string fenced = "```" + seg.language + "\n";
    if (!joined.empty())
    {
        fenced += joined + "\n";
    }
    fenced += "```";
    allBlocks.push_back(DocBlock::MakeCodeBlock(std::move(fenced)));
}

/** @brief Processes a parsed doc segment and handles fallback text segments. */
void ProcessParsedDocSegment(const CommentSegment& seg, std::vector<DocBlock>& allBlocks)
{
    if (seg.lines.empty())
    {
        return;
    }
    size_t beforeCount = allBlocks.size();
    std::string synthetic = WrapInSyntheticComment(seg.lines);
    ParseDocSegment(synthetic, allBlocks);

    // Fallback for tree-sitter-doxygen grammar limitations (e.g. grammar rejects '!' in text):
    if (seg.kind == CommentSegmentKind::Text && allBlocks.size() == beforeCount)
    {
        std::string rawJoined;
        for (size_t i = 0; i < seg.lines.size(); ++i)
        {
            if (i > 0)
            {
                rawJoined += "\n";
            }
            rawJoined += seg.lines[i];
        }
        std::string cleaned = CleanDescriptionLines(RewriteAtInlineCommands(rawJoined));
        if (!cleaned.empty())
        {
            allBlocks.push_back(DocBlock::MakeBody(std::move(cleaned)));
        }
    }
}
} // namespace

std::string RenderDoxygenMarkdown(const std::string& rawComment)
{
    auto contentLines = ExtractCommentLines(rawComment);
    if (contentLines.empty() ||
        std::all_of(contentLines.begin(), contentLines.end(), [](const std::string& l) { return Trim(l).empty(); }))
    {
        return "";
    }

    std::string briefText = ExtractBrief(contentLines);
    auto segments = SegmentCommentLines(contentLines);
    std::vector<DocBlock> allBlocks;

    for (auto& seg : segments)
    {
        if (seg.kind == CommentSegmentKind::VerbatimCode)
        {
            ProcessVerbatimCodeSegment(seg, allBlocks);
        }
        else
        {
            ProcessParsedDocSegment(seg, allBlocks);
        }
    }

    return AssembleMarkdown(std::move(briefText), allBlocks);
}
} // namespace angel_lsp::analysis
