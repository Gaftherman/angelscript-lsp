#include "features/semantic_tokens/SemanticTokensInternal.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

std::vector<std::string_view> SplitLinesView(std::string_view str)
{
    std::vector<std::string_view> lines;
    size_t start = 0;
    for (size_t i = 0; i < str.size(); ++i)
    {
        if (str[i] == '\n')
        {
            size_t len = i - start;
            if (len > 0 && str[i - 1] == '\r')
            {
                len--;
            }
            lines.emplace_back(str.data() + start, len);
            start = i + 1;
        }
    }
    if (start < str.size())
    {
        size_t len = str.size() - start;
        if (len > 0 && str.back() == '\r')
        {
            len--;
        }
        lines.emplace_back(str.data() + start, len);
    }
    return lines;
}

void EmitTemplatePunctuationTokens(const analysis::IndexedHighlightCapture& capture, TokenMeta meta,
                                   const std::vector<std::string_view>& sourceLines, std::vector<RawToken>& rawTokens)
{
    const uint32_t startRow = capture.startLine;
    const uint32_t startCol = capture.startColumn;
    const uint32_t endRow = capture.endLine;
    const uint32_t endCol = capture.endColumn;

    if (startRow < sourceLines.size() && startCol < sourceLines[startRow].size())
    {
        rawTokens.push_back(RawToken{startRow, startCol, 1, meta.tokenType, meta.tokenMod, meta.priority});
    }
    if (endCol > 0 && endRow < sourceLines.size() && (endCol - 1) < sourceLines[endRow].size())
    {
        rawTokens.push_back(RawToken{endRow, endCol - 1, 1, meta.tokenType, meta.tokenMod, meta.priority});
    }
}

void EmitTokenOrSplitLines(const analysis::IndexedHighlightCapture& capture, TokenMeta meta,
                           const std::vector<std::string_view>& sourceLines, std::vector<RawToken>& rawTokens)
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
        if (startRow < sourceLines.size())
        {
            const uint32_t lineLen = static_cast<uint32_t>(sourceLines[startRow].size());
            const uint32_t sc = std::min(startCol, lineLen);
            const uint32_t ec = std::min(endCol, lineLen);
            if (ec > sc)
            {
                rawTokens.push_back(RawToken{startRow, sc, ec - sc, meta.tokenType, meta.tokenMod, meta.priority});
            }
        }
    }
    else if (meta.tokenType != analysis::Type_Operator)
    {
        for (uint32_t r = startRow; r <= endRow; ++r)
        {
            if (r >= sourceLines.size())
            {
                break;
            }
            const uint32_t lineLen = static_cast<uint32_t>(sourceLines[r].size());
            const uint32_t sc = std::min((r == startRow) ? startCol : 0u, lineLen);
            const uint32_t ec = std::min((r == endRow) ? endCol : lineLen, lineLen);
            if (ec > sc)
            {
                rawTokens.push_back(RawToken{r, sc, ec - sc, meta.tokenType, meta.tokenMod, meta.priority});
            }
        }
    }
}

void CollectHighlightsTokens(const TokenRefinementContext& ctx, const std::vector<std::string_view>& sourceLines,
                             std::vector<RawToken>& rawTokens)
{
    for (const auto& capture : ctx.request.facts.captures)
    {
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

void FilterInvalidOperators(std::vector<RawToken>& tokens, const std::vector<std::string_view>& sourceLines)
{
    std::erase_if(tokens,
                  [&sourceLines](const RawToken& tok)
                  {
                      if (tok.tokenType == analysis::Type_Operator)
                      {
                          if (tok.line >= sourceLines.size() ||
                              tok.startChar + tok.length > sourceLines[tok.line].size())
                          {
                              return true;
                          }
                          std::string_view slice(sourceLines[tok.line].data() + tok.startChar, tok.length);
                          if (analysis::IsPunctuationOrBracket(slice) || !analysis::IsGenuineOperator(slice))
                          {
                              return true;
                          }
                      }
                      return false;
                  });
}

} // namespace angel_lsp::features
