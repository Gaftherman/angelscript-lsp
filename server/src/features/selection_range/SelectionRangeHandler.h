#pragma once

#include "analysis/index/NodeIndex.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input positions for resolving selection ranges.
 */
struct SelectionRangeRequest
{
    /**
     * @brief Raw text of the document being queried.
     */
    std::string_view sourceCode;

    /**
     * @brief Cursor positions decoded into UTF-8 line and character coordinates.
     */
    const std::vector<lsp::Position>& positions;

    /**
     * @brief Pre-computed syntax node index from the first semantic pass.
     */
    const analysis::NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Generates hierarchical syntactic selection ranges enclosing each specified cursor position.
 * @param[in] request Input context containing document text, positions, and node index.
 * @return Selection range hierarchy for each requested position in corresponding order, or empty if index is missing.
 */
std::vector<lsp::SelectionRange> GetSelectionRanges(const SelectionRangeRequest& request);
} // namespace angel_lsp::features
