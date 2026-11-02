#pragma once

#include <lsp/messages.h>
#include <lsp/types.h>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input positions for a selection range request.
 */
struct SelectionRangeRequest
{
    std::string sourceCode;
    TSTree* tree = nullptr;

    /**
     * @brief Cursor positions, already decoded into UTF-8 line/character pairs.
     */
    const std::vector<lsp::Position>& positions;
};

/**
 * @brief Builds the expand-selection chain for each requested position.
 *        The editor's "expand selection" walks outward one syntactic step at a time, and the tree
 * @param request Source text, tree, and the positions to answer for.
 * @return One SelectionRange per requested position, in the same order. Empty when there is no
 *        tree to read.
 */
std::vector<lsp::SelectionRange> GetSelectionRanges(const SelectionRangeRequest& request);
} // namespace angel_lsp::features
