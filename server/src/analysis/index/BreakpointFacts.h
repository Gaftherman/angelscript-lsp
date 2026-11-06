#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Evaluatable expression range and text boundary offsets.
 */
struct IndexedEvaluatableSpan
{
    SourceRange range{};
    uint32_t startByte = 0;
    uint32_t endByte = 0;
};
static_assert(sizeof(IndexedEvaluatableSpan) <= sizeof(SourceRange) + 8);

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

/**
 * @brief Resolves the evaluatable expression span at a specific line and column.
 * @param[in] index The AST node index to query.
 * @param[in] line 0-based line coordinate.
 * @param[in] column 0-based column coordinate.
 * @return Evaluatable expression span with source coordinates and byte offsets if found.
 */
std::optional<IndexedEvaluatableSpan> FindEvaluatableSpan(const NodeIndex& index, uint32_t line, uint32_t column);

} // namespace angel_lsp::analysis
