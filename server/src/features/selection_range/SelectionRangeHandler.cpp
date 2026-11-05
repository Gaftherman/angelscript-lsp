#include "features/selection_range/SelectionRangeHandler.h"

#include "features/common/FeatureSharedUtils.h"

#include <memory>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Constructs a parent-linked selection range hierarchy from an innermost-first sequence of ranges.
 * @param[in] ranges Deduplicated sequence of enclosing ranges ordered from innermost to outermost.
 * @return Hierarchically nested SelectionRange starting at the innermost range.
 */
static lsp::SelectionRange AssembleRangeChain(const std::vector<analysis::SourceRange>& ranges)
{
    if (ranges.empty())
    {
        return lsp::SelectionRange{};
    }

    // Build from outermost inward so each child node takes ownership of its enclosing parent.
    lsp::SelectionRange chain{common::ToLspRange(ranges.back()), nullptr};
    for (size_t i = ranges.size() - 1; i > 0; --i)
    {
        lsp::SelectionRange inner{common::ToLspRange(ranges[i - 1]),
                                  std::make_unique<lsp::SelectionRange>(std::move(chain))};
        chain = std::move(inner);
    }
    return chain;
}

std::vector<lsp::SelectionRange> GetSelectionRanges(const SelectionRangeRequest& request)
{
    std::vector<lsp::SelectionRange> result;
    if (!request.nodeIndex || request.positions.empty())
    {
        return result;
    }

    result.reserve(request.positions.size());
    for (const auto& position : request.positions)
    {
        const auto ranges = request.nodeIndex->FindEnclosingSelectionRanges(position.line, position.character);
        result.push_back(AssembleRangeChain(ranges));
    }
    return result;
}
} // namespace angel_lsp::features
