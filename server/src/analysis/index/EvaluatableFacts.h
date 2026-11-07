#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <optional>

namespace angel_lsp::analysis
{

/**
 * @brief Evaluatable expression range and text boundary offsets.
 */
struct IndexedEvaluatableSpan
{
    SourceRange range{};    ///< 0-based source coordinate range.
    uint32_t startByte = 0; ///< Start byte offset within the document text.
    uint32_t endByte = 0;   ///< End byte offset within the document text.
};

/**
 * @brief Resolves the evaluatable expression span at a specific line and column.
 * @param[in] index The AST node index to query.
 * @param[in] line 0-based line coordinate.
 * @param[in] column 0-based column coordinate.
 * @return Evaluatable expression span with source coordinates and byte offsets if found.
 */
std::optional<IndexedEvaluatableSpan> FindEvaluatableSpan(const NodeIndex& index, uint32_t line, uint32_t column);

} // namespace angel_lsp::analysis
