#include "analysis/formatting/FormattingEngine.h"

#include "analysis/formatting/ClangFormatReader.h"
#include "analysis/formatting/FormattingLineBuilder.h"
#include "analysis/formatting/FormattingRenderer.h"
#include "analysis/formatting/FormattingTokens.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::formatting
{

FormatCodeOptions ResolveEffectiveFormatOptions(std::string_view uri, FormatCodeOptions opts)
{
    if (auto clangStyle = LoadClangFormatForFile(uri))
    {
        ApplyClangFormatStyle(opts, *clangStyle);
    }
    return opts;
}

/**
 * @brief Joins the rendered lines `firstIdx..lastIdx` into the replacement text of a range edit.
 * @param[in] outputLines Rendered lines of the whole document.
 * @param[in] firstIdx First rendered line kept.
 * @param[in] lastIdx Last rendered line kept.
 * @return Joined lines, or an empty string when the range is empty.
 */
static std::string ExtractMatchedRangeText(const std::vector<std::string>& outputLines, size_t firstIdx, size_t lastIdx)
{
    std::vector<std::string> rangeLines;
    rangeLines.reserve(lastIdx - firstIdx + 1);
    for (size_t i = firstIdx; i <= lastIdx; ++i)
    {
        rangeLines.push_back(outputLines[i]);
    }
    return rangeLines.empty() ? "" : JoinLines(rangeLines, 0, static_cast<uint32_t>(rangeLines.size() - 1));
}

std::string FormatSourceCode(std::string_view sourceCode, const FormatCodeOptions& formatOptions)
{
    if (sourceCode.empty())
    {
        return "";
    }
    if (formatOptions.disableFormat)
    {
        return std::string(sourceCode);
    }

    std::string_view bom = ExtractBom(sourceCode);
    auto tokens = Tokenize(sourceCode);
    if (tokens.empty())
    {
        return "";
    }

    auto lines = BuildFormattedLines(tokens, formatOptions);
    auto rendered = RenderLines(lines, tokens, formatOptions);
    CollapseAndTrimLines(rendered, formatOptions);
    return AssembleFormattedText(bom, rendered, formatOptions.options);
}

std::string FormatSourceCode(std::string_view sourceCode, const FormatLayoutOptions& options, BraceStyle braceStyle,
                             bool spacesInsideParentheses)
{
    return FormatSourceCode(
        sourceCode, FormatCodeOptions{options, braceStyle, spacesInsideParentheses, false, PointerAlignment::Left});
}

std::optional<FormattedEdit> FormatWholeDocument(std::string_view sourceCode, const FormatCodeOptions& opts)
{
    std::string formatted = FormatSourceCode(sourceCode, opts);
    if (formatted == sourceCode)
    {
        return std::nullopt;
    }

    uint32_t lineCount = 0;
    size_t lastLineLen = 0;
    for (size_t i = 0; i < sourceCode.size(); ++i)
    {
        if (sourceCode[i] == '\n')
        {
            lineCount++;
            lastLineLen = 0;
        }
        else if (sourceCode[i] != '\r')
        {
            lastLineLen++;
        }
    }

    return FormattedEdit{0, 0, lineCount, static_cast<uint32_t>(lastLineLen), std::move(formatted)};
}

std::optional<FormattedEdit> FormatLineRange(std::string_view sourceCode, uint32_t startLine, uint32_t endLine,
                                             const FormatCodeOptions& opts)
{
    auto origLines = SplitLines(sourceCode, true);
    uint32_t totalLines = static_cast<uint32_t>(origLines.size());
    startLine = std::min(startLine, totalLines > 0 ? totalLines - 1 : 0u);
    endLine = std::min(endLine, totalLines > 0 ? totalLines - 1 : 0u);

    if (startLine == 0 && endLine >= totalLines - 1)
    {
        return FormatWholeDocument(sourceCode, opts);
    }

    auto tokens = Tokenize(sourceCode);
    if (tokens.empty())
    {
        return std::nullopt;
    }

    auto lines = BuildFormattedLines(tokens, opts);
    auto outputLines = RenderLines(lines, tokens, opts);

    auto matched = FindMatchedRange(lines, tokens, startLine, endLine);
    if (!matched)
    {
        return std::nullopt;
    }

    uint32_t effStartLine = std::min(matched->actualStartLine, totalLines > 0 ? totalLines - 1 : 0u);
    uint32_t effEndLine = std::min(matched->actualEndLine, totalLines > 0 ? totalLines - 1 : 0u);

    std::string formattedRangeText = ExtractMatchedRangeText(outputLines, matched->firstIdx, matched->lastIdx);
    std::string origRangeText = JoinLines(origLines, effStartLine, effEndLine);

    if (formattedRangeText == origRangeText)
    {
        return std::nullopt;
    }

    return FormattedEdit{effStartLine, 0, effEndLine, static_cast<uint32_t>(origLines[effEndLine].size()),
                         std::move(formattedRangeText)};
}

} // namespace angel_lsp::analysis::formatting
