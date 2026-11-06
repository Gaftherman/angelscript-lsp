#include "BreakpointLocationsHandler.h"

#include "analysis/index/BreakpointFacts.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::features
{

std::vector<BreakpointLocationItem> BreakpointLocationsHandler::Collect(const BreakpointLocationsRequest& request)
{
    std::vector<BreakpointLocationItem> results;
    if (!request.nodeIndex || request.startLine > request.endLine)
    {
        return results;
    }

    const auto spans = analysis::CollectBreakpointSpans(*request.nodeIndex, request.startLine, request.endLine);
    results.reserve(spans.size());

    for (const auto& span : spans)
    {
        if (request.excludedRanges != nullptr &&
            angel_lsp::utils::IsLineExcluded(*request.excludedRanges, span.startLine))
        {
            continue;
        }
        results.push_back(BreakpointLocationItem{span.startLine, span.startCharacter, span.endLine, span.endCharacter});
    }

    std::sort(results.begin(), results.end(),
              [](const BreakpointLocationItem& a, const BreakpointLocationItem& b) noexcept
              {
                  if (a.line != b.line)
                      return a.line < b.line;
                  if (a.column != b.column)
                      return a.column < b.column;
                  if (a.endLine != b.endLine)
                      return a.endLine < b.endLine;
                  return a.endColumn < b.endColumn;
              });

    auto isDuplicate = [](const BreakpointLocationItem& a, const BreakpointLocationItem& b) noexcept
    { return a.line == b.line && a.column == b.column && a.endLine == b.endLine && a.endColumn == b.endColumn; };
    results.erase(std::unique(results.begin(), results.end(), isDuplicate), results.end());

    return results;
}

} // namespace angel_lsp::features
