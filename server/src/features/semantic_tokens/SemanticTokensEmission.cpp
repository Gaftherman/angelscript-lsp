#include "features/semantic_tokens/SemanticTokensInternal.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Checks whether a highlight capture overlaps the requested LSP range.
 * @param[in] capture AST-free highlight capture item.
 * @param[in] range Target LSP coordinate range.
 * @return True if the capture overlaps the range.
 */
static bool CaptureOverlapsRange(const analysis::IndexedHighlightCapture& capture, const lsp::Range& range) noexcept
{
    if (capture.endLine < range.start.line ||
        (capture.endLine == range.start.line && capture.endColumn <= range.start.character))
    {
        return false;
    }
    if (capture.startLine > range.end.line ||
        (capture.startLine == range.end.line && capture.startColumn >= range.end.character))
    {
        return false;
    }
    return true;
}

SourceLineWindow SplitLinesWindow(std::string_view str, const std::optional<lsp::Range>& range)
{
    SourceLineWindow window;
    const uint32_t startLine = range.has_value() ? range->start.line : 0u;
    const uint32_t endLine = range.has_value() ? range->end.line : UINT32_MAX;
    window.baseLine = startLine;

    if (startLine > endLine)
    {
        return window;
    }

    uint32_t line = 0;
    size_t pos = 0;
    while (pos < str.size())
    {
        const size_t nl = str.find('\n', pos);
        const size_t lineEnd = (nl == std::string_view::npos) ? str.size() : nl;
        if (line >= startLine)
        {
            size_t len = lineEnd - pos;
            if (len > 0 && str[pos + len - 1] == '\r')
            {
                --len;
            }
            window.lines.emplace_back(str.data() + pos, len);
        }
        if (nl == std::string_view::npos || line >= endLine)
        {
            break;
        }
        pos = nl + 1;
        ++line;
    }
    return window;
}

void EmitTemplatePunctuationTokens(const analysis::IndexedHighlightCapture& capture, TokenMeta meta,
                                   const SourceLineWindow& sourceLines, std::vector<RawToken>& rawTokens)
{
    const uint32_t startRow = capture.startLine;
    const uint32_t startCol = capture.startColumn;
    const uint32_t endRow = capture.endLine;
    const uint32_t endCol = capture.endColumn;

    if (sourceLines.HasLine(startRow) && startCol < sourceLines.GetLine(startRow).size())
    {
        rawTokens.push_back(RawToken{startRow, startCol, 1, meta.tokenType, meta.tokenMod, meta.priority});
    }
    if (endCol > 0 && sourceLines.HasLine(endRow) && (endCol - 1) < sourceLines.GetLine(endRow).size())
    {
        rawTokens.push_back(RawToken{endRow, endCol - 1, 1, meta.tokenType, meta.tokenMod, meta.priority});
    }
}

void EmitTokenOrSplitLines(const analysis::IndexedHighlightCapture& capture, TokenMeta meta,
                           const SourceLineWindow& sourceLines, std::vector<RawToken>& rawTokens)
{
    if (meta.tokenType == analysis::Type_TemplatePunctuation)
    {
        EmitTemplatePunctuationTokens(capture, meta, sourceLines, rawTokens);
        return;
    }

    const uint32_t startRow = capture.startLine;
    const uint32_t startCol = capture.startColumn;
    const uint32_t endRow = capture.endLine;
    const uint32_t endCol = capture.endColumn;

    if (startRow == endRow)
    {
        if (sourceLines.HasLine(startRow))
        {
            const uint32_t lineLen = static_cast<uint32_t>(sourceLines.GetLine(startRow).size());
            const uint32_t sc = std::min(startCol, lineLen);
            const uint32_t ec = std::min(endCol, lineLen);
            if (ec > sc)
            {
                rawTokens.push_back(RawToken{startRow, sc, ec - sc, meta.tokenType, meta.tokenMod, meta.priority});
            }
        }
    }
    else if (meta.tokenType != analysis::Type_Operator && !sourceLines.lines.empty())
    {
        const uint32_t maxWindowLine = sourceLines.baseLine + static_cast<uint32_t>(sourceLines.lines.size()) - 1;
        const uint32_t rBegin = std::max(startRow, sourceLines.baseLine);
        const uint32_t rEnd = std::min(endRow, maxWindowLine);
        for (uint32_t r = rBegin; r <= rEnd; ++r)
        {
            const uint32_t lineLen = static_cast<uint32_t>(sourceLines.GetLine(r).size());
            const uint32_t sc = std::min((r == startRow) ? startCol : 0u, lineLen);
            const uint32_t ec = std::min((r == endRow) ? endCol : lineLen, lineLen);
            if (ec > sc)
            {
                rawTokens.push_back(RawToken{r, sc, ec - sc, meta.tokenType, meta.tokenMod, meta.priority});
            }
        }
    }
}

void CollectHighlightsTokens(const TokenRefinementContext& ctx, const SourceLineWindow& sourceLines,
                             std::vector<RawToken>& rawTokens)
{
    const auto& range = ctx.request.range;
    for (const auto& capture : ctx.request.facts.captures)
    {
        if (range.has_value() && !CaptureOverlapsRange(capture, *range))
        {
            continue;
        }
        uint32_t tokenType = capture.tokenType;
        tokenType = RefineCapturedTokenType(capture, tokenType, ctx);
        if (tokenType == UINT32_MAX)
        {
            continue;
        }
        if (tokenType == analysis::Type_Operator && capture.isTemplatePunctuationNode)
        {
            tokenType = analysis::Type_TemplatePunctuation;
        }

        TokenMeta meta{tokenType, capture.tokenModifiers, capture.priority};
        if (meta.tokenType == analysis::Type_TemplatePunctuation)
        {
            EmitTemplatePunctuationTokens(capture, meta, sourceLines, rawTokens);
        }
        else
        {
            EmitTokenOrSplitLines(capture, meta, sourceLines, rawTokens);
        }
    }
}

std::vector<RawToken> DeduplicateTokens(std::vector<RawToken>& rawTokens)
{
    std::sort(rawTokens.begin(), rawTokens.end(),
              [](const RawToken& a, const RawToken& b)
              {
                  if (a.line != b.line)
                      return a.line < b.line;
                  if (a.startChar != b.startChar)
                      return a.startChar < b.startChar;
                  return a.priority > b.priority;
              });

    std::vector<RawToken> filtered;
    filtered.reserve(rawTokens.size());
    uint32_t currentLine = UINT32_MAX;
    uint32_t lastEndChar = 0;

    for (const auto& tok : rawTokens)
    {
        if (tok.length == 0)
        {
            continue;
        }
        if (tok.line != currentLine)
        {
            currentLine = tok.line;
            lastEndChar = 0;
        }
        if (tok.startChar >= lastEndChar)
        {
            filtered.push_back(tok);
            lastEndChar = tok.startChar + tok.length;
        }
    }
    return filtered;
}

void NarrowTokensToRange(std::vector<RawToken>& tokens, const lsp::Range& range)
{
    if (tokens.empty())
    {
        return;
    }

    auto first = std::lower_bound(tokens.begin(), tokens.end(), range.start.line,
                                  [](const RawToken& tok, uint32_t line) { return tok.line < line; });

    while (first != tokens.end() && first->line == range.start.line &&
           first->startChar + first->length <= range.start.character)
    {
        ++first;
    }

    auto last = std::upper_bound(first, tokens.end(), range.end.line,
                                 [](uint32_t line, const RawToken& tok) { return line < tok.line; });

    while (last != first)
    {
        auto prev = std::prev(last);
        if (prev->line == range.end.line && prev->startChar >= range.end.character)
        {
            last = prev;
        }
        else
        {
            break;
        }
    }

    if (first >= last)
    {
        tokens.clear();
    }
    else
    {
        std::vector<RawToken> narrowed;
        narrowed.reserve(static_cast<size_t>(last - first));
        for (auto it = first; it != last; ++it)
        {
            narrowed.push_back(std::move(*it));
        }
        tokens = std::move(narrowed);
    }
}

void FilterInvalidOperators(std::vector<RawToken>& tokens, const SourceLineWindow& sourceLines)
{
    std::erase_if(tokens,
                  [&sourceLines](const RawToken& tok)
                  {
                      if (tok.tokenType == analysis::Type_Operator)
                      {
                          const std::string_view lineText = sourceLines.GetLine(tok.line);
                          if (!sourceLines.HasLine(tok.line) || tok.startChar + tok.length > lineText.size())
                          {
                              return true;
                          }
                          std::string_view slice(lineText.data() + tok.startChar, tok.length);
                          if (analysis::IsPunctuationOrBracket(slice) || !analysis::IsGenuineOperator(slice))
                          {
                              return true;
                          }
                      }
                      return false;
                  });
}

} // namespace angel_lsp::features
