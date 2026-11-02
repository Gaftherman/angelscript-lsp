#include "analysis/documentation/DoxygenMarkdown.h"
#include "document/Document.h"
#include "parser/DoxygenParser.h"
#include "utils/Utils.h"

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
static inline std::string Trim(std::string_view str)
{
    return std::string(utils::TrimView(str));
}

/**
 * @brief Strips leading ASCII whitespace.
 * @param[in] str Source text to trim.
 * @return Sub-string with leading whitespace removed.
 */
static std::string TrimLeading(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return "";
    }
    return str.substr(first);
}

/**
 * @brief Strips trailing ASCII whitespace in place.
 * @param[in,out] str String buffer modified in place.
 */
static void TrimTrailing(std::string& str)
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

/**
 * @brief Converts ASCII string to lowercase.
 * @param[in] str Source string to convert.
 * @return Transformed lowercase string.
 */
static std::string ToLower(const std::string& str)
{
    std::string out = str;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/**
 * @brief Returns true if string contains at least one ASCII alphanumeric character.
 * @param[in] text String view to inspect.
 * @return True if alphanumeric characters are present; false otherwise.
 */
static bool HasAlphanumeric(std::string_view text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return std::isalnum(c); });
}

/**
 * @brief Strips trailing stray semicolons from stub comments outside verbatim blocks.
 * @param[in] line Comment line text to process.
 * @return Processed line with trailing semicolon removed if applicable.
 */
static std::string StripTrailingStubSemicolon(std::string_view line)
{
    std::string s(line);
    size_t last = s.find_last_not_of(" \t\r\n");
    if (last == std::string::npos)
    {
        return "";
    }
    if (s[last] == ';')
    {
        size_t prev = s.find_last_not_of(" \t\r\n;", last);
        if (prev != std::string::npos &&
            (s[prev] == '.' || s[prev] == ')' || s[prev] == ']' || s[prev] == '\"' || s[prev] == '\''))
        {
            s.erase(last);
            TrimTrailing(s);
        }
    }
    return s;
}

/**
 * @brief Splits text into lines, stripping trailing carriage returns.
 * @param[in] str Multiline text buffer.
 * @return Vector of split line strings.
 */
static std::vector<std::string> SplitLines(const std::string& str)
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
static std::string StripLineLeadingStar(const std::string& line)
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

/**
 * @brief Checks if a line defines a key-value or option assignment item.
 * @param[in] line Line text to evaluate.
 * @return True if line matches option or assignment pattern; false otherwise.
 */
static bool IsOptionOrAssignmentLine(std::string_view line)
{
    if (!line.empty() && (line.front() == '\'' || line.front() == '"' || line.front() == '`' || line.front() == '['))
    {
        return line.find(" = ") != std::string_view::npos || line.find(": ") != std::string_view::npos ||
               line.find(" - ") != std::string_view::npos;
    }
    size_t eqPos = line.find(" = ");
    return eqPos != std::string_view::npos && eqPos > 0 && eqPos <= 32;
}

/**
 * @brief Checks if a line starts with a parenthesized list prefix like (1) or (a).
 * @param[in] line Candidate line view.
 * @return True if prefix is parenthesized list marker; false otherwise.
 */
static bool IsParenthesizedListPrefix(std::string_view line)
{
    if (line.size() < 4 || !line.starts_with('('))
    {
        return false;
    }
    size_t closeParen = line.find(')');
    if (closeParen == std::string_view::npos || closeParen > 4 || closeParen + 1 >= line.size())
    {
        return false;
    }
    return line[closeParen + 1] == ' ';
}

/**
 * @brief Checks if a line starts with a digit-prefixed item like 1. or 1).
 * @param[in] line Candidate line view.
 * @return True if prefix matches numbered item format; false otherwise.
 */
static bool IsDigitsListPrefix(std::string_view line)
{
    if (line.size() < 3 || !std::isdigit(static_cast<unsigned char>(line[0])))
    {
        return false;
    }
    size_t pos = 1;
    while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos])))
    {
        ++pos;
    }
    if (pos >= line.size() || (line[pos] != '.' && line[pos] != ')'))
    {
        return false;
    }
    if (pos + 1 >= line.size())
    {
        return false;
    }
    return line[pos + 1] == ' ' || line[pos + 1] == '\t';
}

/**
 * @brief Checks if a line starts with a numbered list prefix.
 * @param[in] line Line text to evaluate.
 * @return True if line begins with a recognized numbered marker; false otherwise.
 */
static bool IsNumberedListLine(std::string_view line)
{
    return IsDigitsListPrefix(line) || IsParenthesizedListPrefix(line);
}

/**
 * @brief Checks if a cleaned line represents a Markdown bullet, numbered item, or option definition.
 * @param[in] line Trimmed line text.
 * @return True if line is a Markdown list item; false otherwise.
 */
static bool IsMarkdownListLine(std::string_view line)
{
    if (line.starts_with("* ") || line.starts_with("- ") || line.starts_with("+ ") || line.starts_with("> ") ||
        line.starts_with("\xe2\x80\xa2 "))
    {
        return true;
    }
    return IsNumberedListLine(line) || IsOptionOrAssignmentLine(line);
}

/**
 * @brief Transforms `-# ` Doxygen numbered list items into numbered Markdown list items.
 * @param[in,out] line Line string to replace prefix in.
 * @param[in,out] listNum Current list counter incremented per item.
 */
static void ProcessNumberedListItem(std::string& line, int& listNum)
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
 * @brief Determines whether a newline should be preserved between two consecutive description lines.
 * @param[in] prev Preceding line content.
 * @param[in] curr Following line content.
 * @return True if line break must be retained; false if lines can flow together.
 */
static bool ShouldKeepDescriptionNewline(std::string_view prev, std::string_view curr)
{
    if (IsMarkdownListLine(curr) || IsMarkdownListLine(prev))
    {
        return true;
    }
    if (prev.ends_with(':'))
    {
        return true;
    }
    bool prevEndsWithSentencePunct =
        prev.ends_with('.') || prev.ends_with('!') || prev.ends_with('?') || prev.ends_with(';');
    bool curStartsSentence = !curr.empty() && (std::isupper(static_cast<unsigned char>(curr[0])) ||
                                               std::isdigit(static_cast<unsigned char>(curr[0])) || curr[0] == '\'' ||
                                               curr[0] == '"' || curr[0] == '`' || curr[0] == '[');
    return prevEndsWithSentencePunct && curStartsSentence;
}

/**
 * @brief Cleans description lines by stripping line decorations, converting `-#` list items,
 * and joining lines with single spaces or linebreaks (for lists and punctuated clauses).
 */
static std::string CleanDescriptionLines(const std::string& raw)
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
            result += ShouldKeepDescriptionNewline(cleaned[i - 1], cleaned[i]) ? "\n" : " ";
        }
        result += cleaned[i];
    }
    return Trim(result);
}

/**
 * @brief Converts common inline HTML tags (<code>, <tt>, <b>, <strong>, <i>, <em>) to Markdown.
 */
static std::string RewriteInlineHtml(const std::string& text)
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

/**
 * @brief Checks if character is a Doxygen escaped character.
 * @param[in] c Character to inspect.
 * @return True if character is escapable in Doxygen; false otherwise.
 */
static bool IsDoxygenEscapedChar(char c)
{
    return c == '@' || c == '\\' || c == '$' || c == '&' || c == '#' || c == '%' || c == '<' || c == '>';
}

/**
 * @brief Tries to consume a Doxygen escape sequence (\@, \\, @@, etc.).
 * @param[in] text Source text being scanned.
 * @param[in,out] i Current character offset advanced on match.
 * @param[in,out] result Accumulator receiving unescaped characters.
 * @return True if an escape sequence was consumed; false otherwise.
 */
static bool TryConsumeInlineEscape(std::string_view text, size_t& i, std::string& result)
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

/**
 * @brief Checks if a character precedes an inline command as whitespace or opening punctuation.
 * @param[in] text Source string buffer.
 * @param[in] i Index of command start.
 * @return True if preceding character qualifies as boundary; false otherwise.
 */
static bool IsInlineCommandPrefix(std::string_view text, size_t i)
{
    if (i == 0)
    {
        return true;
    }
    char prev = text[i - 1];
    return std::isspace(static_cast<unsigned char>(prev)) || prev == '(' || prev == '[' || prev == '{' || prev == '<' ||
           prev == '"' || prev == '\'';
}

/**
 * @brief Checks if trailing character is punctuation that should remain outside formatting.
 * @param[in] c Character under test.
 * @return True if character is trailing punctuation; false otherwise.
 */
static bool IsTrailingPunctuation(char c)
{
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == ')' || c == ']' || c == '}';
}

/**
 * @brief Matches known inline commands (ref, em, c, p, b, a, e) at position i.
 * @param[in] text Source string view.
 * @param[in] i Offset of command indicator.
 * @return MatchedInlineCommand structure describing length and command kind.
 */
static MatchedInlineCommand MatchInlineCommandName(std::string_view text, size_t i)
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

/**
 * @brief Formats an inline command and its argument into Markdown.
 */
static std::string FormatInlineCommand(std::string_view cmd, std::string_view arg)
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

/**
 * @brief Trims trailing punctuation from argument slice [argStart, argEnd).
 * @param[in] text Source string buffer.
 * @param[in] argStart Start offset of argument.
 * @param[in] argEnd End offset of argument.
 * @return Adjusted end offset excluding trailing punctuation.
 */
static size_t TrimArgTrailingPunctuation(std::string_view text, size_t argStart, size_t argEnd)
{
    size_t trailingStart = argEnd;
    while (trailingStart > argStart && IsTrailingPunctuation(text[trailingStart - 1]))
    {
        --trailingStart;
    }
    return trailingStart > argStart ? trailingStart : argEnd;
}

/**
 * @brief Tries to rewrite an inline command at position i.
 * @param[in] text Source text buffer.
 * @param[in,out] i Scan index advanced past command.
 * @param[in,out] result Output Markdown accumulator.
 * @return True if an inline command was recognized and transformed; false otherwise.
 */
static bool TryRewriteInlineCommand(const std::string& text, size_t& i, std::string& result)
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
static std::string RewriteAtInlineCommands(const std::string& input)
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
static bool IsKnownDoxygenCommandStartingWithN(std::string_view text)
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

/**
 * @brief Checks whether position i in line is an unescaped literal newline command.
 * @param[in] line Line string being examined.
 * @param[in] i Offset of backslash character.
 * @return True if position represents a literal newline escape; false otherwise.
 */
static bool IsEscapedNewline(const std::string& line, size_t i)
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
static std::vector<std::string> SplitCommentLineOnNewlines(const std::string& line)
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

/**
 * @brief Tries to process opening line of a C-style block comment. Returns true if block remains open.
 * @param[in] line First line of block comment.
 * @param[in,out] contents Extracted comment content accumulator.
 * @return True if comment block continues across multiple lines; false if closed on first line.
 */
static bool TryProcessFirstBlockLine(const std::string& line, std::vector<std::string>& contents)
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

/**
 * @brief Extracts content lines from a multi-line C-style comment block.
 * @param[in] rawLines Raw source lines comprising block comment.
 * @return Stripped lines without comment framing delimiters.
 */
static std::vector<std::string> ExtractBlockCommentLines(const std::vector<std::string>& rawLines)
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

/**
 * @brief Extracts content lines from single-line C++ comment runs.
 * @param[in] rawLines Raw consecutive single-line comment strings.
 * @return Normalized comment body lines.
 */
static std::vector<std::string> ExtractLineCommentLines(const std::vector<std::string>& rawLines)
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

/**
 * @brief Splits comment contents on newline escapes outside verbatim blocks.
 * @param[in] contents Lines of comment body.
 * @return Expanded line list with embedded escapes resolved.
 */
static std::vector<std::string> SplitCommentVerbatimAndNewlines(const std::vector<std::string>& contents)
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
            for (size_t k = 0; k < splitLines.size(); ++k)
            {
                if (k > 0)
                {
                    finalContents.push_back("");
                }
                finalContents.push_back(StripTrailingStubSemicolon(splitLines[k]));
            }
        }
    }
    return finalContents;
}

/**
 * @brief Extracts the content lines of a raw comment, whichever way it was written.
 */
static std::vector<std::string> ExtractCommentLines(const std::string& rawComment)
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

/**
 * @brief Checks if a command name is an inline formatting command.
 * @param[in] cmd Command name identifier.
 * @return True if command is an inline formatting tag; false otherwise.
 */
static bool IsDoxygenInlineCommandName(std::string_view cmd)
{
    return cmd == "b" || cmd == "e" || cmd == "em" || cmd == "a" || cmd == "c" || cmd == "p" || cmd == "n";
}

/**
 * @brief Checks if a command name is a verbatim code block boundary command.
 * @param[in] cmd Command name identifier.
 * @return True if command starts or ends verbatim section; false otherwise.
 */
static bool IsDoxygenVerbatimCommandName(std::string_view cmd)
{
    return cmd == "code" || cmd == "endcode" || cmd == "verbatim" || cmd == "endverbatim";
}

/**
 * @brief Checks if a line begins with a Doxygen block command (@name or \name).
 */
static bool IsBlockCommand(std::string_view line, size_t& cmdEnd)
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

/**
 * @brief Checks if a character is a closing quotation mark or bracket.
 * @param[in] c Character to test.
 * @return True if character is closing delimiter; false otherwise.
 */
static bool IsClosingQuoteOrBracket(char c)
{
    return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}';
}

/**
 * @brief Checks if dot at position i marks a sentence boundary.
 * @param[in] line Source line text.
 * @param[in] i Offset of dot character.
 * @return True if dot terminates a sentence; false otherwise.
 */
static bool IsDotSentenceBoundary(std::string_view line, size_t i)
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

/**
 * @brief Finds the first sentence-ending dot in a line not part of numbers or ellipsis.
 * @param[in] line Text line to search.
 * @return Byte offset of sentence terminator, or std::string_view::npos if none.
 */
static size_t FindSentenceEndingDot(std::string_view line)
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

/**
 * @brief Checks if a line starts with an explicit brief command.
 * @param[in] trimmed Whitespace-trimmed line string.
 * @return True if line starts with brief tag; false otherwise.
 */
static bool IsExplicitBriefStart(std::string_view trimmed)
{
    if (trimmed.starts_with("@brief") || trimmed.starts_with("\\brief") || trimmed.starts_with("@short") ||
        trimmed.starts_with("\\short"))
    {
        return trimmed.size() == 6 || std::isspace(static_cast<unsigned char>(trimmed[6]));
    }
    return false;
}

/**
 * @brief Checks if a continuation line should terminate brief absorption.
 * @param[in] nextTrimmed Whitespace-trimmed subsequent line text.
 * @return True if continuation line signals the end of the brief description; false otherwise.
 */
static bool ShouldStopBriefContinuation(std::string_view nextTrimmed)
{
    if (nextTrimmed.empty() || nextTrimmed.starts_with("@code") || nextTrimmed.starts_with("\\code") ||
        nextTrimmed.starts_with("@verbatim") || nextTrimmed.starts_with("\\verbatim"))
    {
        return true;
    }
    size_t cmdEnd = 0;
    return IsBlockCommand(nextTrimmed, cmdEnd);
}

/**
 * @brief Extracts explicit brief description starting at firstContentIdx.
 * @param[in,out] lines Comment body lines.
 * @param[in] firstContentIdx Index of first non-empty line.
 * @return Extracted brief description text.
 */
static std::string ExtractExplicitBrief(std::vector<std::string>& lines)
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

/**
 * @brief Checks whether a line cannot serve as an implicit brief candidate.
 * @param[in] line Candidate line text view.
 * @return True if line is ineligible to serve as an implicit brief; false otherwise.
 */
static bool IsIneligibleImplicitBriefLine(std::string_view line)
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

/**
 * @brief Splits first content line at sentence-ending dot into brief and remainder.
 * @param[in,out] firstLine Content line modified in place to retain only the remainder.
 * @param[in] dotPos Character offset of the sentence-ending period.
 * @return Substring representing the leading brief sentence.
 */
static std::string SplitFirstLineAtDot(std::string& firstLine, size_t dotPos)
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
    while (!remainder.empty() &&
           (remainder.back() == ';' || std::isspace(static_cast<unsigned char>(remainder.back()))))
    {
        remainder.pop_back();
    }
    bool hasAlnum = std::any_of(remainder.begin(), remainder.end(), [](unsigned char ch) { return std::isalnum(ch); });
    firstLine = hasAlnum ? remainder : "";
    return briefText;
}

/**
 * @brief Finds the index of the first non-empty line in a comment lines collection.
 * @param[in] lines Collection of comment lines to inspect.
 * @return Zero-based index of the first non-empty line, or std::string::npos if all are empty.
 */
static size_t FindFirstNonEmptyLine(const std::vector<std::string>& lines)
{
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (!Trim(lines[i]).empty())
        {
            return i;
        }
    }
    return std::string::npos;
}

/**
 * @brief Determines if the first content line should be extracted as an implicit brief.
 *
 * An implicit brief is only separated from the body if:
 * 1. The first line contains multiple sentences on that line (brief + remainder), OR
 * 2. The first line is followed by an empty line (explicit paragraph break), OR
 * 3. The first line is followed by a Doxygen block tag (e.g. @param, @return).
 */
static bool ShouldExtractImplicitBrief(const std::vector<std::string>& lines, size_t firstContentIdx)
{
    if (firstContentIdx >= lines.size())
    {
        return false;
    }
    std::string_view firstLine = lines[firstContentIdx];
    size_t dotPos = FindSentenceEndingDot(firstLine);
    if (dotPos != std::string::npos)
    {
        size_t nextPos = dotPos + 1;
        while (nextPos < firstLine.size() && (IsClosingQuoteOrBracket(firstLine[nextPos]) || firstLine[nextPos] == ';'))
        {
            ++nextPos;
        }
        std::string_view remainder = firstLine.substr(nextPos);
        bool hasAlnum = std::any_of(remainder.begin(), remainder.end(),
                                    [](unsigned char ch) { return std::isalnum(static_cast<unsigned char>(ch)); });
        if (hasAlnum)
        {
            return true;
        }
    }

    if (firstContentIdx + 1 < lines.size())
    {
        std::string_view nextLine = lines[firstContentIdx + 1];
        if (Trim(std::string(nextLine)).empty())
        {
            return true;
        }
        size_t cmdEnd = 0;
        if (IsBlockCommand(TrimLeading(std::string(nextLine)), cmdEnd))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Extracts implicit brief from the first sentence or first content line.
 * @param[in,out] lines Comment body lines.
 * @param[in] firstContentIdx Index of first content line.
 * @return Inferred brief summary string.
 */
static std::string ExtractImplicitBrief(std::vector<std::string>& lines, size_t firstContentIdx)
{
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
    size_t last = briefText.find_last_not_of(" \t\r\n");
    if (last != std::string::npos && briefText[last] == ';')
    {
        size_t prev = briefText.find_last_not_of(" \t\r\n;", last);
        if (prev != std::string::npos && (briefText[prev] == '.' || briefText[prev] == ')' || briefText[prev] == ']'))
        {
            briefText.erase(last);
        }
    }
    return CleanDescriptionLines(briefText);
}

/**
 * @brief Extracts the brief description in the pre-pass and removes it from lines.
 */
static std::string ExtractBrief(std::vector<std::string>& lines)
{
    std::string brief = ExtractExplicitBrief(lines);
    if (!brief.empty())
    {
        return brief;
    }

    size_t firstContentIdx = FindFirstNonEmptyLine(lines);
    if (firstContentIdx == std::string::npos || !ShouldExtractImplicitBrief(lines, firstContentIdx))
    {
        return "";
    }

    return ExtractImplicitBrief(lines, firstContentIdx);
}

/**
 * @brief Flushes any active segment in the segmenter state.
 * @param[in,out] state Active segmenter state.
 * @param[in,out] segments Destination segment collection.
 */
static void FlushActiveSegment(SegmenterState& state, std::vector<CommentSegment>& segments)
{
    if (state.hasActiveSeg && !state.currentSeg.lines.empty())
    {
        segments.push_back(std::move(state.currentSeg));
    }
    state.currentSeg = CommentSegment{};
    state.hasActiveSeg = false;
}

/**
 * @brief Checks if a line starts a verbatim code block and determines its command length.
 * @param[in] line Line string view to check.
 * @param[out] cmdLen Receives length of matched command.
 * @param[out] isVerbatim Receives true if block is verbatim environment.
 * @return True if line initiates code block; false otherwise.
 */
static bool IsVerbatimCodeStart(std::string_view line, size_t& cmdLen, bool& isVerbatim)
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

/**
 * @brief Checks if a line ends a verbatim code block.
 * @param[in] line Line string view to inspect.
 * @return True if line closes verbatim code environment; false otherwise.
 */
static bool IsVerbatimCodeEnd(std::string_view line)
{
    std::string_view trimmed = line;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
    {
        trimmed.remove_prefix(1);
    }
    return trimmed.starts_with("@endcode") || trimmed.starts_with("\\endcode") || trimmed.starts_with("@endverbatim") ||
           trimmed.starts_with("\\endverbatim");
}

/**
 * @brief Parses language identifier from code block command arguments.
 * @param[in] afterCmd Slice following the code command keyword.
 * @return Parsed code language token, e.g. angelscript or cpp.
 */
static std::string ParseCodeLanguage(std::string_view afterCmd)
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

/**
 * @brief Processes a line when parser is inside a verbatim code block. Returns true if handled.
 * @param[in] line Current code block line.
 * @param[in,out] state Active segmenter state tracking code block nesting.
 * @param[in,out] segments Destination segment list.
 * @return True if line was processed as part of code block; false otherwise.
 */
static bool TryProcessCodeLine(const std::string& line, SegmenterState& state, std::vector<CommentSegment>& segments)
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

/**
 * @brief Checks and initiates a verbatim code block segment if line matches code start syntax.
 * @param[in] trimmedLeading Line with leading whitespace stripped.
 * @param[in,out] state Active segmenter state.
 * @param[in,out] segments Destination segment list.
 * @return True if code block was initiated; false otherwise.
 */
static bool TryStartCodeBlock(std::string_view trimmedLeading, SegmenterState& state,
                              std::vector<CommentSegment>& segments)
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

/**
 * @brief Appends content line to active tag or text segment.
 * @param[in] line Line text to append.
 * @param[in,out] state Active segmenter state.
 * @param[in,out] segments Accumulated segments.
 */
static void AppendTextOrTagLine(const std::string& line, SegmenterState& state, std::vector<CommentSegment>& segments)
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

/**
 * @brief Processes a single line inside the segmenter loop.
 * @param[in] line Current line string.
 * @param[in,out] state Active segmenter state.
 * @param[in,out] segments Accumulated segments.
 */
static void ProcessSegmentLine(const std::string& line, SegmenterState& state, std::vector<CommentSegment>& segments)
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
static std::vector<CommentSegment> SegmentCommentLines(const std::vector<std::string>& lines)
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

/**
 * @brief Wraps lines into a synthetic block comment for parsing.
 * @param[in] lines Collection of comment lines.
 * @return Synthetic C-style comment block string suitable for AST parsing.
 */
static std::string WrapInSyntheticComment(const std::vector<std::string>& lines)
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

/**
 * @brief Helper to find a child node with a matching type name.
 * @param[in] node Parent AST node.
 * @param[in] typeName Tree-Sitter grammatical type name to match.
 * @return First matching child TSNode, or null TSNode if not found.
 */
static TSNode FindChildByType(TSNode node, const char* typeName)
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

/**
 * @brief Renders a code_word inline node to Markdown.
 */
static std::string RenderCodeWordNode(TSNode node, const std::string& sourceCode)
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

/**
 * @brief Renders an emphasis inline node to Markdown.
 */
static std::string RenderEmphasisNode(TSNode node, const std::string& sourceCode)
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

/**
 * @brief Renders a link inline node to Markdown.
 * @param[in] node AST link node to render.
 * @param[in] sourceCode Underlying source text buffer.
 * @return Rendered Markdown link string.
 */
static std::string RenderLinkNode(TSNode node, const std::string& sourceCode)
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
static std::string RenderInlineNode(TSNode node, const std::string& sourceCode)
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
static std::string RenderDescription(TSNode descNode, const std::string& sourceCode)
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
static std::string ProcessBriefHeader(TSNode briefNode, const std::string& sourceCode)
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
static std::string ProcessCodeBlock(TSNode blockNode, const std::string& sourceCode)
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

/**
 * @brief Normalizes storageclass bracketed parameter direction text.
 * @param[in] rawSc Raw storage class string view containing bracketed direction.
 * @return Normalized direction string ("in", "out", "in,out"), or empty string if invalid.
 */
static std::string NormalizeParamDirection(std::string_view rawSc)
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

/**
 * @brief Extracts parameter direction, names, and description from tag children.
 * @param[in] tagNode AST node representing the parameter tag.
 * @param[in] sourceCode Source text buffer of the document.
 * @param[in,out] item Target ParamItem receiving extracted data.
 */
static void ProcessParamTagChildren(TSNode tagNode, const std::string& sourceCode, ParamItem& item)
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
 * @param[in] tagNode AST node representing the parameter tag.
 * @param[in] sourceCode Source text buffer of the document.
 * @return Structured ParamItem populated from the tag node.
 */
static ParamItem ProcessParamTag(TSNode tagNode, const std::string& sourceCode)
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
 * @param[in] tagNode AST node representing the tparam tag.
 * @param[in] sourceCode Source text buffer of the document.
 * @return Structured TParamItem populated from the tag node.
 */
static TParamItem ProcessTParamTag(TSNode tagNode, const std::string& sourceCode)
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

/**
 * @brief Extracts the description string for return tags.
 * @param[in] tagNode AST node representing the return tag.
 * @param[in] sourceCode Source text buffer of the document.
 * @return Extracted return description text.
 */
static std::string ProcessReturnTag(TSNode tagNode, const std::string& sourceCode)
{
    TSNode descChild = FindChildByType(tagNode, "description");
    if (!ts_node_is_null(descChild))
    {
        return RenderDescription(descChild, sourceCode);
    }
    return "";
}

/**
 * @brief Extracts description text from an admonition tag node.
 * @param[in] tagNode AST node representing the admonition tag.
 * @param[in] sourceCode Source text buffer of the document.
 * @return Extracted admonition description text.
 */
static std::string ExtractAdmonitionDescription(TSNode tagNode, const std::string& sourceCode)
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

/**
 * @brief Maps a cleaned Doxygen admonition tag name to its canonical Markdown label.
 * @param[in] cleanedTag Cleaned lowercase tag name without prefix.
 * @return Canonical title label for the admonition block.
 */
static std::string MapAdmonitionLabel(std::string_view cleanedTag)
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
 * @param[in] cleanedTag Cleaned tag name string.
 * @param[in] tagNode AST node representing the admonition tag.
 * @param[in] sourceCode Source text buffer of the document.
 * @return DocBlock configured as an admonition.
 */
static DocBlock ProcessAdmonitionTag(const std::string& cleanedTag, TSNode tagNode, const std::string& sourceCode)
{
    std::string desc = Trim(ExtractAdmonitionDescription(tagNode, sourceCode));
    std::string label = MapAdmonitionLabel(cleanedTag);
    return DocBlock::MakeAdmonition(std::move(label), std::move(desc));
}

/**
 * @brief Extracts value and description from a @retval tag.
 * @param[in] tagNode AST node representing the retval tag.
 * @param[in] sourceCode Source text buffer of the document.
 * @return Structured RetValItem populated from the tag node.
 */
static RetValItem ProcessRetValTag(TSNode tagNode, const std::string& sourceCode)
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
 * @param[in] tag Tag name string view to inspect.
 * @return True if the tag is a structural entity declaration tag; false otherwise.
 */
static bool IsStructuralTag(std::string_view tag)
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

/**
 * @brief Extracts documentation lines from multi-line structural tags.
 * @param[in] lines Vector of text lines within the structural tag.
 * @return Cleaned single-string description.
 */
static std::string ExtractMultiLineStructuralDescription(const std::vector<std::string>& lines)
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

/**
 * @brief Extracts descriptive text after single-line structural entity identifier.
 * @param[in] raw Raw line string view containing the structural command.
 * @return Description text following the entity identifier, or empty if none.
 */
static std::string ExtractSingleLineStructuralDescription(std::string_view raw)
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
 * @param[in] tagNode AST node representing the structural tag.
 * @param[in] sourceCode Source text buffer of the document.
 * @return Description text extracted from the tag node.
 */
static std::string ProcessStructuralTagDescription(TSNode tagNode, const std::string& sourceCode)
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

/**
 * @brief Appends text to a string block with proper spacing.
 * @param[in,out] target String being appended to.
 * @param[in] addition Content to append.
 */
static void AppendToBlockText(std::string& target, const std::string& addition)
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

/**
 * @brief Appends text to a DocBlock target.
 * @param[in,out] block Target documentation block.
 * @param[in] text Text chunk to append.
 */
static void AppendToDocBlock(DocBlock& block, const std::string& text)
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

/**
 * @brief Splits an inline tag description into target word and trailing remainder.
 * @param[in] desc Tag description view.
 * @return Pair of target identifier and remaining descriptive text.
 */
static std::pair<std::string, std::string> SplitInlineTagWord(std::string_view desc)
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

/**
 * @brief Formats an inline word into Markdown depending on tag command.
 */
static std::string FormatInlineWord(std::string_view cmd, std::string_view word)
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
static void MergeInlineTag(const std::string& cmd, TSNode tagNode, const std::string& sourceCode,
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

/**
 * @brief Extracts raw tag name from a tag AST node.
 * @param[in] child AST node of tag construct.
 * @param[in] syntheticDoc Synthetic comment source buffer.
 * @return Extracted raw tag identifier string.
 */
static std::string ExtractRawTagName(TSNode child, const std::string& syntheticDoc)
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

/**
 * @brief Tries to dispatch common inline, parameter, return, or retval tags. Returns true if handled.
 * @param[in] lowerTag Normalized lowercase tag identifier.
 * @param[in] child AST node representing tag.
 * @param[in] syntheticDoc Synthetic doc source text.
 * @param[in,out] blocks Accumulated documentation blocks.
 * @return True if tag was recognized and dispatched; false otherwise.
 */
static bool TryDispatchCommonTag(std::string_view lowerTag, TSNode child, const std::string& syntheticDoc,
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

/**
 * @brief Dispatches tag AST nodes to corresponding block builders.
 * @param[in] child Tag AST node.
 * @param[in] syntheticDoc Synthetic comment source text.
 * @param[in,out] blocks Destination documentation blocks.
 */
static void DispatchSegmentTagNode(TSNode child, const std::string& syntheticDoc, std::vector<DocBlock>& blocks)
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
    else if (lowerTag == "link")
    {
        std::string desc = Trim(ExtractAdmonitionDescription(child, syntheticDoc));
        if (desc.ends_with("@endlink"))
        {
            desc = Trim(desc.substr(0, desc.size() - 8));
        }
        else if (desc.ends_with("\\endlink"))
        {
            desc = Trim(desc.substr(0, desc.size() - 8));
        }
        blocks.push_back(DocBlock::MakeBody(desc.empty() ? "@link" : "@link " + desc));
    }
    else if (lowerTag == "endlink")
    {
        return;
    }
    else if (!rawTagName.empty())
    {
        blocks.push_back(ProcessAdmonitionTag(rawTagName, child, syntheticDoc));
    }
}

/**
 * @brief Appends or merges rendered description AST node into doc blocks.
 * @param[in] child Description AST node.
 * @param[in] syntheticDoc Synthetic comment source text.
 * @param[in,out] blocks Destination documentation blocks.
 */
static void DispatchDescriptionChildNode(TSNode child, const std::string& syntheticDoc, std::vector<DocBlock>& blocks)
{
    std::string body = RenderDescription(child, syntheticDoc);
    if (body.empty() || !HasAlphanumeric(body))
    {
        return;
    }
    if (!blocks.empty() && blocks.back().kind == DocBlockKind::Body)
    {
        bool keepNewline = ShouldKeepDescriptionNewline(blocks.back().text, body);
        blocks.back().text += (keepNewline ? "\n" : " ") + body;
    }
    else
    {
        blocks.push_back(DocBlock::MakeBody(std::move(body)));
    }
}

/**
 * @brief Appends raw text line child node into doc blocks.
 * @param[in] child Text line AST node.
 * @param[in] syntheticDoc Synthetic comment source text.
 * @param[in,out] blocks Destination documentation blocks.
 */
static void DispatchTextLineChildNode(TSNode child, const std::string& syntheticDoc, std::vector<DocBlock>& blocks)
{
    std::string line = std::string(parser::DoxygenParser::GetNodeText(child, syntheticDoc));
    line = CleanDescriptionLines(RewriteAtInlineCommands(line));
    if (line.empty() || !HasAlphanumeric(line))
    {
        return;
    }
    if (!blocks.empty() && blocks.back().kind == DocBlockKind::Body)
    {
        AppendToBlockText(blocks.back().text, line);
    }
    else
    {
        blocks.push_back(DocBlock::MakeBody(std::move(line)));
    }
}

/**
 * @brief Dispatches a child AST node of a parsed segment.
 * @param[in] child Child AST node from segment root.
 * @param[in] syntheticDoc Synthetic comment source text.
 * @param[in,out] blocks Destination documentation blocks.
 */
static void DispatchSegmentChildNode(TSNode child, const std::string& syntheticDoc, std::vector<DocBlock>& blocks)
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
        DispatchDescriptionChildNode(child, syntheticDoc, blocks);
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
        DispatchTextLineChildNode(child, syntheticDoc, blocks);
    }
}

/**
 * @brief Parses a single synthetic document segment and collects AST DocBlocks.
 */
static void ParseDocSegment(const std::string& syntheticDoc, std::vector<DocBlock>& blocks)
{
    if (Trim(syntheticDoc).empty())
    {
        return;
    }

    parser::DoxygenParser parser;
    document::TreePtr tree = document::MakeTreePtr(parser.Parse(syntheticDoc));
    if (!tree)
    {
        return;
    }

    TSNode root = ts_tree_root_node(tree.get());
    if (ts_node_is_null(root))
    {
        return;
    }

    uint32_t childCount = ts_node_child_count(root);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        DispatchSegmentChildNode(ts_node_child(root, i), syntheticDoc, blocks);
    }
}

/**
 * @brief Assembles brief and general body paragraphs/code blocks into output sections.
 * @param[in] briefText Summary brief text.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleBriefAndBody(std::string briefText, const std::vector<DocBlock>& blocks,
                                 std::vector<std::string>& sections)
{
    size_t firstBriefBlockIdx = std::string::npos;
    if (!briefText.empty() && HasAlphanumeric(briefText))
    {
        sections.push_back(std::move(briefText));
    }
    else
    {
        for (size_t i = 0; i < blocks.size(); ++i)
        {
            if (blocks[i].kind == DocBlockKind::Brief && !blocks[i].text.empty() && HasAlphanumeric(blocks[i].text))
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
        if ((b.kind == DocBlockKind::Body || b.kind == DocBlockKind::Brief) && !b.text.empty() &&
            HasAlphanumeric(b.text))
        {
            sections.push_back(b.text);
        }
        else if (b.kind == DocBlockKind::CodeBlock && !b.text.empty())
        {
            sections.push_back(b.text);
        }
    }
}

/**
 * @brief Assembles template parameter bullets into output sections.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleTParamBullets(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
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

/**
 * @brief Assembles parameter bullets into output sections.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleParamBullets(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
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

/**
 * @brief Assembles return and retval sections into output sections.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleReturnSection(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
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

/**
 * @brief Assembles admonitions in source order into output sections.
 * @param[in] blocks Parsed documentation blocks.
 * @param[in,out] sections Output Markdown sections.
 */
static void AssembleAdmonitions(const std::vector<DocBlock>& blocks, std::vector<std::string>& sections)
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
static std::string AssembleMarkdown(std::string briefText, const std::vector<DocBlock>& blocks)
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

/**
 * @brief Processes a verbatim code comment segment and appends a code block.
 * @param[in,out] seg Code comment segment.
 * @param[in,out] allBlocks Output documentation blocks.
 */
static void ProcessVerbatimCodeSegment(CommentSegment& seg, std::vector<DocBlock>& allBlocks)
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

/**
 * @brief Processes a parsed doc segment and handles fallback text segments.
 * @param[in] seg Parsed comment segment.
 * @param[in,out] allBlocks Output documentation blocks.
 */
static void ProcessParsedDocSegment(const CommentSegment& seg, std::vector<DocBlock>& allBlocks)
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
