#pragma once

#include "analysis/index/IsolationSiteFacts.h"
#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Ancestor context entry tracked during flat cursor traversal for identifier classification.
 */
struct IsolationAncestorFrame
{
    const void* targetId = nullptr; ///< Callee, member, or declaration name node handle.
    uint32_t depth = 0;             ///< Cursor tree depth at which this ancestor node resides.
    uint32_t boundaryByte = 0;      ///< Declarator initializer start byte or declaration name start byte.
    TSSymbol sym = 0;               ///< Tree-sitter grammar symbol identifier of the ancestor.
    bool hasBoundary = false;       ///< True when a variable declarator carries a non-null initializer value.
};

/**
 * @brief Converts a syntax node's start and end points to a diagnostic range.
 * @param[in] node Syntax node to measure.
 * @return Line and column span of the node, or a zero range if null.
 */
[[nodiscard]] inline SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const TSPoint s = ts_node_start_point(node);
    const TSPoint e = ts_node_end_point(node);
    return {s.row, s.column, e.row, e.column};
}

/**
 * @brief Converts a syntax node to its half-open byte span.
 * @param[in] node Syntax node to measure.
 * @return Start and end byte of the node, or a zero range if null.
 */
[[nodiscard]] inline IndexedByteRange BytesOf(TSNode node) noexcept
{
    return ts_node_is_null(node) ? IndexedByteRange{}
                                 : IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)};
}

/**
 * @brief Resolves the enclosing class name and shared-isolation status for a source offset.
 * @param[in] index Precomputed syntax node index.
 * @param[in] offset Document byte offset to inspect.
 * @param[in] source Document source text buffer.
 * @return Pair containing the enclosing class name (or empty) and whether the context is shared.
 */
[[nodiscard]] std::pair<std::string_view, bool> EnclosingContext(const NodeIndex& index, uint32_t offset,
                                                                 std::string_view source) noexcept;

/**
 * @brief Collects isolation identifier site facts in a single iterative cursor pass from the tree root.
 * @param[in] index Precomputed syntax node index providing the root node, comment spans, and enclosing lookups.
 * @param[in] source Document source text buffer.
 * @param[in,out] identifiers Destination vector receiving classified identifier site facts.
 */
void CollectIsolationIdentifierFacts(const NodeIndex& index, std::string_view source,
                                     std::vector<IsolationIdentifierSiteFact>& identifiers);

} // namespace angel_lsp::analysis
