#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Finds the byte range of the receiver expression written before a member-access dot.
 *        Uses the `object` of an enclosing member expression, else a clean preceding sibling, else the
 *        widest ancestor ending before the dot that crosses no expression or statement boundary.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] sourceCode Document text the index was built from.
 * @param[in] dotByteOffset Byte offset of the `.` being completed.
 * @return Byte range of the receiver expression, or std::nullopt when the index has no tree, the dot is
 *         at offset 0, or no receiver can be recovered.
 * @note Cost: up to two O(depth) descents plus an ancestor climb bounded by the tree depth.
 */
[[nodiscard]] std::optional<IndexedByteRange>
FindMemberReceiverRange(const NodeIndex& index, std::string_view sourceCode, uint32_t dotByteOffset);

/**
 * @brief Tells whether a position sits directly in a class or interface body, outside any member function.
 *        Climbing from the node at the position stops at the first function declaration or statement block
 *        (the position is inside a callable) or at the first class or interface declaration (it is not).
 * @param[in] index Node index of the current syntax revision.
 * @param[in] line 0-based line of the position.
 * @param[in] column 0-based column of the position, as used for a Tree-Sitter point.
 * @return True when a class or interface declaration is reached first; false otherwise, including when
 *         the index has no tree.
 * @note Cost: one O(depth) descent plus an ancestor climb bounded by the tree depth.
 */
[[nodiscard]] bool IsInClassBodyOutsideCallable(const NodeIndex& index, uint32_t line, uint32_t column);

} // namespace angel_lsp::analysis
