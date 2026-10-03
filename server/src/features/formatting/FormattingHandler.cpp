#include "features/formatting/FormattingHandler.h"
#include "features/formatting/ClangFormatReader.h"
#include "features/formatting/FormattingLineBuilder.h"
#include "features/formatting/FormattingRenderer.h"
#include "features/formatting/FormattingTokens.h"
#include <algorithm>

namespace angel_lsp::features
{
namespace
{
FormatCodeOptions ResolveEffectiveFormatOptions(std::string_view uri, FormatCodeOptions opts)
{
    if (auto clangStyle = LoadClangFormatForFile(uri))
    {
        ApplyClangFormatStyle(opts, *clangStyle);
    }
    return opts;
}

std::string ExtractMatchedRangeText(const std::vector<std::string>& outputLines, size_t firstIdx, size_t lastIdx)
{
    std::vector<std::string> rangeLines;
    rangeLines.reserve(lastIdx - firstIdx + 1);
    for (size_t i = firstIdx; i <= lastIdx; ++i)
    {
        rangeLines.push_back(outputLines[i]);
    }
    return rangeLines.empty() ? "" : JoinLines(rangeLines, 0, static_cast<uint32_t>(rangeLines.size() - 1));
}
} // namespace

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

std::string FormatSourceCode(std::string_view sourceCode, const lsp::FormattingOptions& options, BraceStyle braceStyle,
                             bool spacesInsideParentheses)
{
    return FormatSourceCode(
        sourceCode, FormatCodeOptions{options, braceStyle, spacesInsideParentheses, false, PointerAlignment::Left});
}

std::optional<std::vector<lsp::TextEdit>> FormatDocument(const FormattingRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::vector<lsp::TextEdit>{};
    }

    FormatCodeOptions opts = ResolveEffectiveFormatOptions(
        request.uri, FormatCodeOptions{request.options, request.braceStyle, request.spacesInsideParentheses,
                                       request.keepEmptyBlocksOnSingleLine, request.pointerAlignment});
    if (opts.disableFormat)
    {
        return std::vector<lsp::TextEdit>{};
    }

    std::string formatted = FormatSourceCode(request.sourceCode, opts);
    if (formatted == request.sourceCode)
    {
        return std::vector<lsp::TextEdit>{};
    }

    uint32_t lineCount = 0;
    size_t lastLineLen = 0;
    for (size_t i = 0; i < request.sourceCode.size(); ++i)
    {
        if (request.sourceCode[i] == '\n')
        {
            lineCount++;
            lastLineLen = 0;
        }
        else if (request.sourceCode[i] != '\r')
        {
            lastLineLen++;
        }
    }

    lsp::TextEdit edit;
    edit.range.start = lsp::Position{0, 0};
    edit.range.end = lsp::Position{lineCount, static_cast<uint32_t>(lastLineLen)};
    edit.newText = std::move(formatted);

    return std::vector<lsp::TextEdit>{std::move(edit)};
}

std::optional<std::vector<lsp::TextEdit>> FormatRange(const RangeFormattingRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::vector<lsp::TextEdit>{};
    }

    FormatCodeOptions rangeOpts = ResolveEffectiveFormatOptions(
        request.uri, FormatCodeOptions{request.options, request.braceStyle, request.spacesInsideParentheses,
                                       request.keepEmptyBlocksOnSingleLine, request.pointerAlignment});
    if (rangeOpts.disableFormat)
    {
        return std::vector<lsp::TextEdit>{};
    }

    auto origLines = SplitLines(request.sourceCode, true);
    uint32_t totalLines = static_cast<uint32_t>(origLines.size());
    uint32_t startLine = std::min(request.range.start.line, totalLines > 0 ? totalLines - 1 : 0u);
    uint32_t endLine = std::min(request.range.end.line, totalLines > 0 ? totalLines - 1 : 0u);

    if (startLine == 0 && endLine >= totalLines - 1)
    {
        return FormatDocument(FormattingRequest{request.uri, request.sourceCode, request.tree, rangeOpts.options,
                                                rangeOpts.braceStyle, rangeOpts.spacesInsideParentheses,
                                                rangeOpts.keepEmptyBlocksOnSingleLine, rangeOpts.pointerAlignment});
    }

    auto tokens = Tokenize(request.sourceCode);
    if (tokens.empty())
    {
        return std::vector<lsp::TextEdit>{};
    }

    auto lines = BuildFormattedLines(tokens, rangeOpts);
    auto outputLines = RenderLines(lines, tokens, rangeOpts);

    auto matched = FindMatchedRange(lines, tokens, startLine, endLine);
    if (!matched)
    {
        return std::vector<lsp::TextEdit>{};
    }

    uint32_t effStartLine = std::min(matched->actualStartLine, totalLines > 0 ? totalLines - 1 : 0u);
    uint32_t effEndLine = std::min(matched->actualEndLine, totalLines > 0 ? totalLines - 1 : 0u);

    std::string formattedRangeText = ExtractMatchedRangeText(outputLines, matched->firstIdx, matched->lastIdx);
    std::string origRangeText = JoinLines(origLines, effStartLine, effEndLine);

    if (formattedRangeText == origRangeText)
    {
        return std::vector<lsp::TextEdit>{};
    }

    lsp::TextEdit edit;
    edit.range.start = lsp::Position{effStartLine, 0};
    edit.range.end = lsp::Position{effEndLine, static_cast<uint32_t>(origLines[effEndLine].size())};
    edit.newText = std::move(formattedRangeText);

    return std::vector<lsp::TextEdit>{std::move(edit)};
}

std::optional<std::vector<lsp::TextEdit>> FormatOnType(const OnTypeFormattingRequest& request)
{
    if (request.sourceCode.empty() || (request.ch != ";" && request.ch != "}"))
    {
        return std::nullopt;
    }

    if (request.tree && ts_node_has_error(ts_tree_root_node(request.tree)))
    {
        return std::nullopt;
    }

    uint32_t targetLine = request.position.line;

    RangeFormattingRequest rangeReq{
        request.uri,
        request.sourceCode,
        request.tree,
        lsp::Range{lsp::Position{targetLine, 0}, lsp::Position{targetLine, request.position.character}},
        request.options,
        request.braceStyle,
        request.spacesInsideParentheses,
        request.keepEmptyBlocksOnSingleLine,
        request.pointerAlignment};

    return FormatRange(rangeReq);
}
} // namespace angel_lsp::features