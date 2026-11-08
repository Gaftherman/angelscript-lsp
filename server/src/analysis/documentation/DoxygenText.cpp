#include "analysis/documentation/DoxygenMarkdown.h"
#include "analysis/documentation/DoxygenMarkdownInternal.h"
#include "parser/DoxygenSyntax.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::doxygen
{
std::string Trim(std::string_view str)
{
    return std::string(utils::TrimView(str));
}

std::string TrimLeading(std::string_view str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        return "";
    }
    return std::string(str.substr(first));
}

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

std::string ToLower(std::string_view str)
{
    std::string out(str);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool HasAlphanumeric(std::string_view text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return std::isalnum(c); });
}

std::string StripTrailingStubSemicolon(std::string_view line)
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

std::vector<std::string> SplitLines(std::string_view str)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < str.size())
    {
        const size_t newline = str.find('\n', start);
        const size_t end = newline == std::string_view::npos ? str.size() : newline;
        std::string_view line = str.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        lines.emplace_back(line);
        start = end + 1;
    }
    return lines;
}

std::string StripLineLeadingStar(std::string_view line)
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
    return std::string(line.substr(i));
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

bool ShouldKeepDescriptionNewline(std::string_view prev, std::string_view curr)
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

std::string CleanDescriptionLines(std::string_view raw)
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
static std::string RewriteInlineHtml(std::string_view text)
{
    if (text.find('<') == std::string_view::npos)
    {
        return std::string(text);
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
                    if (closePos != std::string_view::npos)
                    {
                        std::string inner =
                            std::string(text.substr(i + openTag.size(), closePos - (i + openTag.size())));
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

bool IsTrailingPunctuation(char c)
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
static bool TryRewriteInlineCommand(std::string_view text, size_t& i, std::string& result)
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

std::string RewriteAtInlineCommands(std::string_view input)
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
} // namespace angel_lsp::analysis::doxygen
