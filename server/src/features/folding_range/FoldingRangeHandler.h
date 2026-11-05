#pragma once

#include "analysis/index/NodeIndex.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and immutable input parameters for resolving document folding ranges.
 */
struct FoldingRangeRequest
{
    /**
     * @brief Target document URI.
     */
    std::string_view uri;

    /**
     * @brief Raw text of the document being queried.
     */
    std::string_view sourceCode;

    /**
     * @brief Pre-computed syntax node index from the first semantic pass.
     */
    const analysis::NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Tracks contiguous lines of identical folding kind (e.g. comments or imports).
 */
struct FoldingSpanTracker
{
    int start = -1;
    int end = -1;
    std::optional<lsp::FoldingRangeKind> kind;

    /**
     * @brief Updates the span tracker with the match status of the current line.
     * @param[in] match True if current line matches the span type.
     * @param[in] lineIdx Zero-based line number.
     * @param[out] outRanges Destination vector for completed folding ranges.
     */
    void Update(bool match, uint32_t lineIdx, std::vector<lsp::FoldingRange>& outRanges) noexcept
    {
        if (match)
        {
            if (start == -1)
            {
                start = static_cast<int>(lineIdx);
            }
            end = static_cast<int>(lineIdx);
            return;
        }
        if (start != -1 && end > start)
        {
            outRanges.push_back(lsp::FoldingRange{
                .startLine = static_cast<uint32_t>(start),
                .endLine = static_cast<uint32_t>(end),
                .kind = kind,
            });
        }
        start = -1;
        end = -1;
    }
};

/**
 * @brief Aggregates line trackers and stacks for text-based folding collection.
 */
struct FoldingScanState
{
    FoldingSpanTracker comments{-1, -1, lsp::FoldingRangeKind::Comment};
    FoldingSpanTracker imports{-1, -1, lsp::FoldingRangeKind::Imports};
    std::vector<uint32_t> regionStack;
    std::vector<uint32_t> ifStack;
};

/**
 * @brief List of folding ranges extracted for document folding.
 */
using FoldingRangeResult = std::vector<lsp::FoldingRange>;

/**
 * @brief Extracts folding regions (classes, interfaces, namespaces, functions,
 *        control blocks, comments, and preprocessor directives) from the AST or NodeIndex.
 * @param[in] request Immutable context for folding range computation.
 * @return Optional vector of FoldingRange items; std::nullopt if document cannot be parsed.
 */
std::optional<FoldingRangeResult> GetFoldingRanges(const FoldingRangeRequest& request);
} // namespace angel_lsp::features
