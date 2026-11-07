#include "features/formatting/FormattingHandler.h"

#include "analysis/formatting/FormattingEngine.h"
#include "analysis/index/FormattingFacts.h"

#include <utility>

namespace angel_lsp::features
{
analysis::formatting::FormatLayoutOptions ToLayoutOptions(const lsp::FormattingOptions& options)
{
    return analysis::formatting::FormatLayoutOptions{options.tabSize, options.insertSpaces,
                                                     options.trimTrailingWhitespace, options.insertFinalNewline,
                                                     options.trimFinalNewlines};
}

/**
 * @brief Wraps a formatter replacement into the single LSP edit the formatting requests return.
 * @param[in,out] edit Formatter replacement; its text is moved out.
 * @return List holding one text edit with the same range and text.
 */
static std::vector<lsp::TextEdit> ToTextEdits(analysis::formatting::FormattedEdit& edit)
{
    lsp::TextEdit textEdit;
    textEdit.range.start = lsp::Position{edit.startLine, edit.startColumn};
    textEdit.range.end = lsp::Position{edit.endLine, edit.endColumn};
    textEdit.newText = std::move(edit.newText);
    return std::vector<lsp::TextEdit>{std::move(textEdit)};
}

std::optional<std::vector<lsp::TextEdit>> FormatDocument(const FormattingRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::vector<lsp::TextEdit>{};
    }

    const analysis::formatting::FormatCodeOptions opts = analysis::formatting::ResolveEffectiveFormatOptions(
        request.uri,
        analysis::formatting::FormatCodeOptions{request.options, request.braceStyle, request.spacesInsideParentheses,
                                                request.keepEmptyBlocksOnSingleLine, request.pointerAlignment});
    if (opts.disableFormat)
    {
        return std::vector<lsp::TextEdit>{};
    }

    std::optional<analysis::formatting::FormattedEdit> edit =
        analysis::formatting::FormatWholeDocument(request.sourceCode, opts);
    return edit ? ToTextEdits(*edit) : std::vector<lsp::TextEdit>{};
}

std::optional<std::vector<lsp::TextEdit>> FormatRange(const RangeFormattingRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::vector<lsp::TextEdit>{};
    }

    const analysis::formatting::FormatCodeOptions rangeOpts = analysis::formatting::ResolveEffectiveFormatOptions(
        request.uri,
        analysis::formatting::FormatCodeOptions{request.options, request.braceStyle, request.spacesInsideParentheses,
                                                request.keepEmptyBlocksOnSingleLine, request.pointerAlignment});
    if (rangeOpts.disableFormat)
    {
        return std::vector<lsp::TextEdit>{};
    }

    std::optional<analysis::formatting::FormattedEdit> edit = analysis::formatting::FormatLineRange(
        request.sourceCode, request.range.start.line, request.range.end.line, rangeOpts);
    return edit ? ToTextEdits(*edit) : std::vector<lsp::TextEdit>{};
}

std::optional<std::vector<lsp::TextEdit>> FormatOnType(const OnTypeFormattingRequest& request)
{
    if (request.sourceCode.empty() || (request.ch != ";" && request.ch != "}"))
    {
        return std::nullopt;
    }

    if (request.nodeIndex &&
        analysis::IsOnTypeFormattingSuppressed(*request.nodeIndex, request.position.line, request.position.character))
    {
        return std::nullopt;
    }

    uint32_t targetLine = request.position.line;

    RangeFormattingRequest rangeReq{
        request.uri,
        request.sourceCode,
        lsp::Range{lsp::Position{targetLine, 0}, lsp::Position{targetLine, request.position.character}},
        request.options,
        request.braceStyle,
        request.spacesInsideParentheses,
        request.keepEmptyBlocksOnSingleLine,
        request.pointerAlignment};

    return FormatRange(rangeReq);
}
} // namespace angel_lsp::features
