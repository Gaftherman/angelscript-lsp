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
/**
 * @brief Checks whether position i in line is an unescaped literal newline command.
 * @param[in] line Line string being examined.
 * @param[in] i Offset of backslash character.
 * @return True if position represents a literal newline escape; false otherwise.
 */
static bool IsEscapedNewline(std::string_view line, size_t i)
{
    if ((line[i] != '\\' && line[i] != '@') || i + 1 >= line.size() || line[i + 1] != 'n')
    {
        return false;
    }
    if (line[i] == '\\' && i > 0 && line[i - 1] == '\\')
    {
        return false;
    }
    return !IsKnownDoxygenCommandStartingWithN(line.substr(i + 1));
}

/**
 * @brief Splits a comment line into multiple lines at literal \n or @n escapes.
 */
static std::vector<std::string> SplitCommentLineOnNewlines(std::string_view line)
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
            result.push_back(std::string(line.substr(start, chunkEnd - start)));
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
    result.push_back(std::string(line.substr(start)));
    return result;
}

/**
 * @brief Tries to process opening line of a C-style block comment. Returns true if block remains open.
 * @param[in] line First line of block comment.
 * @param[in,out] contents Extracted comment content accumulator.
 * @return True if comment block continues across multiple lines; false if closed on first line.
 */
static bool TryProcessFirstBlockLine(std::string_view line, std::vector<std::string>& contents)
{
    size_t openPos = line.find("/*");
    if (openPos == std::string_view::npos)
    {
        return false;
    }
    size_t start = openPos + 2;
    size_t closePos = line.rfind("*/");
    if (closePos != std::string_view::npos && closePos >= start)
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
    std::string afterOpen = std::string(line.substr(start));
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

std::vector<std::string> ExtractCommentLines(std::string_view rawComment)
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

    std::string cmdName = ToLower(line.substr(1, i - 1));
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
        if (IsBlockCommand(TrimLeading(nextLine), cmdEnd))
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

std::string ExtractBrief(std::vector<std::string>& lines)
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

/**
 * @brief Processes a line when parser is inside a verbatim code block. Returns true if handled.
 * @param[in] line Current code block line.
 * @param[in,out] state Active segmenter state tracking code block nesting.
 * @param[in,out] segments Destination segment list.
 * @return True if line was processed as part of code block; false otherwise.
 */
static bool TryProcessCodeLine(std::string_view line, SegmenterState& state, std::vector<CommentSegment>& segments)
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
    state.currentCode.lines.push_back(std::string(line));
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
static void AppendTextOrTagLine(std::string_view line, SegmenterState& state, std::vector<CommentSegment>& segments)
{
    if (state.hasActiveSeg && state.currentSeg.kind == CommentSegmentKind::Tag)
    {
        state.currentSeg.lines.push_back(std::string(line));
    }
    else
    {
        if (!state.hasActiveSeg || state.currentSeg.kind != CommentSegmentKind::Text)
        {
            FlushActiveSegment(state, segments);
            state.currentSeg.kind = CommentSegmentKind::Text;
            state.hasActiveSeg = true;
        }
        state.currentSeg.lines.push_back(std::string(line));
    }
}

/**
 * @brief Processes a single line inside the segmenter loop.
 * @param[in] line Current line string.
 * @param[in,out] state Active segmenter state.
 * @param[in,out] segments Accumulated segments.
 */
static void ProcessSegmentLine(std::string_view line, SegmenterState& state, std::vector<CommentSegment>& segments)
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

        std::string normLine(line);
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
} // namespace angel_lsp::analysis::doxygen
