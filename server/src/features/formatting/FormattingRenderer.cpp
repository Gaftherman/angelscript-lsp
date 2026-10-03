#include "features/formatting/FormattingRenderer.h"
#include "features/formatting/FormattingSpacing.h"
#include <algorithm>

namespace angel_lsp::features
{
namespace
{
std::string MakeIndent(int level, const FormatCodeOptions& options)
{
    if (level <= 0)
        return "";
    uint32_t tabSize = options.options.tabSize > 0 ? options.options.tabSize : 4;
    if (options.options.insertSpaces)
        return std::string(static_cast<size_t>(level * tabSize), ' ');
    return std::string(static_cast<size_t>(level), '\t');
}

std::string RenderClangFormatOffLine(const LineInfo& line, const std::vector<Token>& tokens)
{
    if (line.tokenIndices.empty())
        return "";
    std::string lineStr(tokens[line.tokenIndices.front()].column, ' ');
    for (size_t k = 0; k < line.tokenIndices.size(); ++k)
    {
        size_t tokIdx = line.tokenIndices[k];
        if (k > 0)
        {
            const auto& prevTok = tokens[line.tokenIndices[k - 1]];
            const auto& currTok = tokens[tokIdx];
            size_t prevEnd = prevTok.column + prevTok.text.size();
            if (currTok.column > prevEnd)
            {
                lineStr.append(currTok.column - prevEnd, ' ');
            }
        }
        lineStr += tokens[tokIdx].text;
    }
    return lineStr;
}
} // namespace

std::vector<std::string> RenderLines(const std::vector<LineInfo>& lines, const std::vector<Token>& tokens,
                                     const FormatCodeOptions& options)
{
    std::vector<std::string> outputLines;
    outputLines.reserve(lines.size());

    for (const auto& line : lines)
    {
        if (line.isBlankLine)
        {
            outputLines.push_back("");
            continue;
        }
        if (line.isClangFormatOff)
        {
            outputLines.push_back(RenderClangFormatOffLine(line, tokens));
            continue;
        }

        std::string lineStr = MakeIndent(line.indentLevel, options);
        for (size_t k = 0; k < line.tokenIndices.size(); ++k)
        {
            size_t tokIdx = line.tokenIndices[k];
            if (k > 0)
            {
                size_t prevTokIdx = line.tokenIndices[k - 1];
                if (tokens[tokIdx].type == TokenType::LineComment && options.spacesBeforeTrailingComments > 1)
                {
                    lineStr.append(options.spacesBeforeTrailingComments, ' ');
                }
                else if (NeedsSpaceBetween(tokens, prevTokIdx, tokIdx, options))
                {
                    lineStr += ' ';
                }
            }
            lineStr += tokens[tokIdx].text;
        }

        if (options.options.trimTrailingWhitespace.value_or(true))
        {
            while (!lineStr.empty() && (lineStr.back() == ' ' || lineStr.back() == '\t'))
            {
                lineStr.pop_back();
            }
        }
        outputLines.push_back(std::move(lineStr));
    }
    return outputLines;
}

void CollapseAndTrimLines(std::vector<std::string>& lines, const FormatCodeOptions& options)
{
    std::vector<std::string> collapsed;
    uint32_t consecutiveBlank = 0;
    const uint32_t maxKeep = options.maxEmptyLinesToKeep;

    for (auto& l : lines)
    {
        if (l.empty())
        {
            if (!collapsed.empty() && consecutiveBlank < maxKeep)
            {
                collapsed.push_back("");
                consecutiveBlank++;
            }
        }
        else
        {
            collapsed.push_back(std::move(l));
            consecutiveBlank = 0;
        }
    }

    if (options.options.trimFinalNewlines.value_or(true))
    {
        while (!collapsed.empty() && collapsed.back().empty())
        {
            collapsed.pop_back();
        }
    }
    lines = std::move(collapsed);
}

std::string AssembleFormattedText(std::string_view bom, const std::vector<std::string>& lines,
                                  const lsp::FormattingOptions& options)
{
    std::string result(bom);
    for (size_t idx = 0; idx < lines.size(); ++idx)
    {
        result += lines[idx];
        if (idx + 1 < lines.size() || options.insertFinalNewline.value_or(true))
        {
            result += '\n';
        }
    }
    return result;
}

std::string_view ExtractBom(std::string_view& sourceCode)
{
    if (sourceCode.size() >= 3 && static_cast<unsigned char>(sourceCode[0]) == 0xEF &&
        static_cast<unsigned char>(sourceCode[1]) == 0xBB && static_cast<unsigned char>(sourceCode[2]) == 0xBF)
    {
        std::string_view bom = sourceCode.substr(0, 3);
        sourceCode.remove_prefix(3);
        return bom;
    }
    return {};
}

std::vector<std::string> SplitLines(std::string_view text, bool keepTrailingEmpty)
{
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(cur);
            cur.clear();
        }
        else if (c != '\r')
        {
            cur += c;
        }
    }
    if (keepTrailingEmpty || !cur.empty())
    {
        lines.push_back(cur);
    }
    return lines;
}

std::string JoinLines(const std::vector<std::string>& lines, uint32_t startLine, uint32_t endLine)
{
    std::string res;
    for (uint32_t i = startLine; i <= endLine && i < lines.size(); ++i)
    {
        if (i > startLine)
        {
            res += '\n';
        }
        res += lines[i];
    }
    return res;
}
} // namespace angel_lsp::features
