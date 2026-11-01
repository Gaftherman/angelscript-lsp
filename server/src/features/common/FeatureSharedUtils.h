#pragma once

#include "analysis/SymbolTable.h"
#include <concepts>
#include <cstdint>
#include <lsp/types.h>
#include <tree_sitter/api.h>

namespace angel_lsp::features::common
{

/**
 * @brief Converts a Tree-Sitter AST node range into an LSP protocol Range.
 * @param[in] node Valid Tree-Sitter AST node.
 * @return LSP Range spanning node start and end points.
 */
[[nodiscard]] inline lsp::Range ToLspRange(TSNode node) noexcept
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return lsp::Range{lsp::Position{start.row, start.column}, lsp::Position{end.row, end.column}};
}

/**
 * @brief Converts any range-like structure with line and character fields into an LSP protocol Range.
 * @tparam T Structure type containing startLine, startCharacter, endLine, and endCharacter.
 * @param[in] item Source range object.
 * @return Converted LSP Range.
 */
template <typename T>
    requires requires(const T& t) {
        { t.startLine } -> std::convertible_to<uint32_t>;
        { t.startCharacter } -> std::convertible_to<uint32_t>;
        { t.endLine } -> std::convertible_to<uint32_t>;
        { t.endCharacter } -> std::convertible_to<uint32_t>;
    }
[[nodiscard]] inline lsp::Range ToLspRange(const T& item) noexcept
{
    return lsp::Range{lsp::Position{item.startLine, item.startCharacter},
                      lsp::Position{item.endLine, item.endCharacter}};
}

/**
 * @brief Checks if two LSP protocol ranges are identical in start and end coordinates.
 * @param[in] a First LSP range.
 * @param[in] b Second LSP range.
 * @return True if start and end positions match exactly; false otherwise.
 */
[[nodiscard]] inline bool SameRange(const lsp::Range& a, const lsp::Range& b) noexcept
{
    return a.start.line == b.start.line && a.start.character == b.start.character && a.end.line == b.end.line &&
           a.end.character == b.end.character;
}

} // namespace angel_lsp::features::common
