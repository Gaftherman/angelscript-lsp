#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Walk state shared by the pass-1 statement classifiers while collecting breakpoint sites.
 * @note Lives only for one CollectBreakpointSpans call; `out` is that call's result vector.
 */
struct BreakpointSpanContext
{
    const parser::GrammarSymbols& gs; ///< Precompiled grammar symbol and field IDs.
    uint32_t startLine = 0;           ///< Inclusive first line a site may start on.
    uint32_t endLine = 0;             ///< Inclusive last line a site may start on.
    std::vector<SourceRange>& out;    ///< Receives every accepted breakpoint site.
    bool inStatementBlock = false;    ///< True when the current declaration sits inside a statement block.
};

/**
 * @brief Collects all valid breakpoint source ranges within [startLine, endLine].
 * @param[in] index The AST node index to walk.
 * @param[in] startLine Inclusive 0-based start line.
 * @param[in] endLine Inclusive 0-based end line.
 * @return Unsorted list of valid breakpoint candidate source ranges.
 */
std::vector<SourceRange> CollectBreakpointSpans(const NodeIndex& index, uint32_t startLine, uint32_t endLine);

} // namespace angel_lsp::analysis
