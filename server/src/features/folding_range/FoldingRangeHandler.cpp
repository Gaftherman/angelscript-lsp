#include "features/folding_range/FoldingRangeHandler.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Strips leading whitespace from a string view without copying.
 * @param[in] s Input string view.
 * @return String view without leading spaces, tabs, or carriage returns.
 */
static std::string_view TrimLeading(std::string_view s) noexcept
{
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r'))
    {
        ++start;
    }
    return s.substr(start);
}

/**
 * @brief Maps the pass-1 foldable spans recorded by NodeIndex into LSP folding ranges.
 * @param[in] nodeIndex Pre-computed syntax node index holding multi-line blocks and block comments.
 * @param[in,out] outRanges Destination accumulator for discovered folding ranges.
 */
static void CollectNodeIndexFoldingRanges(const analysis::NodeIndex& nodeIndex,
                                          std::vector<lsp::FoldingRange>& outRanges)
{
    const auto spans = nodeIndex.FoldableSpans();
    outRanges.reserve(outRanges.size() + spans.size());
    for (const auto& span : spans)
    {
        std::optional<lsp::FoldingRangeKind> kind;
        if (span.kind == analysis::IndexedFoldableKind::Comment)
        {
            kind = lsp::FoldingRangeKind::Comment;
        }
        outRanges.push_back(lsp::FoldingRange{
            .startLine = span.range.startLine,
            .endLine = span.range.endLine,
            .startCharacter = span.range.startCharacter,
            .endCharacter = span.range.endCharacter,
            .kind = kind,
        });
    }
}

/**
 * @brief Pops the last opening directive from the stack and appends a folding range if non-empty.
 * @param[in,out] stack Directive start line stack.
 * @param[in] lineIdx Closing directive line index.
 * @param[in] kind LSP folding range kind.
 * @param[in,out] outRanges Destination vector for completed folding ranges.
 */
static void PopAndRecord(std::vector<uint32_t>& stack, uint32_t lineIdx, std::optional<lsp::FoldingRangeKind> kind,
                         std::vector<lsp::FoldingRange>& outRanges)
{
    if (stack.empty())
    {
        return;
    }
    const uint32_t start = stack.back();
    stack.pop_back();
    if (start < lineIdx)
    {
        outRanges.push_back(lsp::FoldingRange{.startLine = start, .endLine = lineIdx, .kind = kind});
    }
}

/**
 * @brief Checks if a line begins with a specific preprocessor directive keyword, allowing leading whitespace after '#'.
 * @param[in] line Trimmed line text view starting with '#'.
 * @param[in] dir Expected directive keyword (e.g. "region", "if").
 * @return True if the line contains the matching directive followed by whitespace or end of line.
 */
static bool MatchesDirective(std::string_view line, std::string_view dir) noexcept
{
    if (!line.starts_with('#'))
    {
        return false;
    }
    size_t i = 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    {
        ++i;
    }
    if (!line.substr(i).starts_with(dir))
    {
        return false;
    }
    i += dir.size();
    return i >= line.size() || line[i] == ' ' || line[i] == '\t' || line[i] == '\r';
}

/**
 * @brief Checks if a line begins with an import statement or #include directive.
 * @param[in] line Trimmed line text view.
 * @return True if the line matches an import or include directive.
 */
static bool MatchesImport(std::string_view line) noexcept
{
    if (MatchesDirective(line, "include"))
    {
        return true;
    }
    if (line.starts_with("import"))
    {
        return line.size() == 6 || line[6] == ' ' || line[6] == '\t';
    }
    return false;
}

/**
 * @brief Processes preprocessor #region, #if, and matching end directives.
 * @param[in] trimmed Trimmed line text view.
 * @param[in] lineIdx Current line index.
 * @param[in,out] state Active preprocessor and span tracking state.
 * @param[in,out] outRanges Accumulated folding ranges.
 */
static void HandlePreprocessorLine(std::string_view trimmed, uint32_t lineIdx, FoldingScanState& state,
                                   std::vector<lsp::FoldingRange>& outRanges)
{
    if (MatchesDirective(trimmed, "region"))
    {
        state.regionStack.push_back(lineIdx);
    }
    else if (MatchesDirective(trimmed, "endregion"))
    {
        PopAndRecord(state.regionStack, lineIdx, lsp::FoldingRangeKind::Region, outRanges);
    }
    else if (MatchesDirective(trimmed, "if") || MatchesDirective(trimmed, "ifdef") ||
             MatchesDirective(trimmed, "ifndef"))
    {
        state.ifStack.push_back(lineIdx);
    }
    else if (MatchesDirective(trimmed, "endif"))
    {
        PopAndRecord(state.ifStack, lineIdx, std::nullopt, outRanges);
    }
}

/**
 * @brief Collects comment, import, and preprocessor folding ranges in a single zero-allocation text pass.
 * @param[in] source Document source text buffer.
 * @param[in,out] outRanges Accumulated folding ranges.
 */
static void CollectTextBasedFoldingRanges(std::string_view source, std::vector<lsp::FoldingRange>& outRanges)
{
    FoldingScanState state;
    size_t start = 0;
    uint32_t lineIdx = 0;
    while (start < source.size())
    {
        size_t end = source.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = source.size();
        }
        size_t len = end - start;
        if (len > 0 && source[start + len - 1] == '\r')
        {
            --len;
        }
        const std::string_view trimmed = TrimLeading(source.substr(start, len));
        state.comments.Update(trimmed.starts_with("//"), lineIdx, outRanges);
        state.imports.Update(MatchesImport(trimmed), lineIdx, outRanges);
        HandlePreprocessorLine(trimmed, lineIdx, state, outRanges);

        start = (end < source.size()) ? end + 1 : source.size();
        ++lineIdx;
    }

    state.comments.Update(false, lineIdx, outRanges);
    state.imports.Update(false, lineIdx, outRanges);
}

/**
 * @brief In-place deduplication and ordering of folding ranges by start line ascending and end line descending.
 * @param[in,out] rawRanges Candidate folding ranges to compact and sort.
 * @return Deduplicated and sorted folding ranges.
 */
static FoldingRangeResult DeduplicateAndSortFoldingRanges(std::vector<lsp::FoldingRange>& rawRanges)
{
    rawRanges.erase(std::remove_if(rawRanges.begin(), rawRanges.end(),
                                   [](const lsp::FoldingRange& fr) noexcept { return fr.startLine >= fr.endLine; }),
                    rawRanges.end());

    std::stable_sort(rawRanges.begin(), rawRanges.end(),
                     [](const lsp::FoldingRange& a, const lsp::FoldingRange& b) noexcept
                     {
                         if (a.startLine != b.startLine)
                         {
                             return a.startLine < b.startLine;
                         }
                         if (a.endLine != b.endLine)
                         {
                             return a.endLine > b.endLine;
                         }
                         return a.kind.has_value() && !b.kind.has_value();
                     });

    const auto it = std::unique(rawRanges.begin(), rawRanges.end(),
                                [](const lsp::FoldingRange& a, const lsp::FoldingRange& b) noexcept
                                { return a.startLine == b.startLine && a.endLine == b.endLine; });
    rawRanges.erase(it, rawRanges.end());

    return std::move(rawRanges);
}

std::optional<FoldingRangeResult> GetFoldingRanges(const FoldingRangeRequest& request)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::FoldingRange> rawRanges;
    CollectNodeIndexFoldingRanges(*request.nodeIndex, rawRanges);
    CollectTextBasedFoldingRanges(request.sourceCode, rawRanges);

    return DeduplicateAndSortFoldingRanges(rawRanges);
}
} // namespace angel_lsp::features
