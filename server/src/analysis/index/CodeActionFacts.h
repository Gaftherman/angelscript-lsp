#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace angel_lsp::analysis
{

/**
 * @brief Byte offsets and source coordinate bounds of an indexed AST node.
 */
struct IndexedNodeSpan
{
    /// Half-open byte range within document text.
    IndexedByteRange bytes;
    /// Line and character range within document text.
    SourceRange range;
};

/**
 * @brief Finds the byte range and source coordinates of the descendant node at a point.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] line 0-based line of the target point.
 * @param[in] column 0-based column of the target point.
 * @return Span of the descendant node if present; std::nullopt if the tree or node is null.
 * @note Cost: O(depth) descent in the syntax tree.
 */
[[nodiscard]] std::optional<IndexedNodeSpan> FindNodeSpanAtPoint(const NodeIndex& index, uint32_t line,
                                                                 uint32_t column);

/**
 * @brief Syntactic context for removing an unused variable declarator or declaration.
 */
struct IndexedDeclaratorRemoval
{
    /// The position lies in a variable_declaration.
    bool inDeclaration = false;
    /// Number of variable_declarator children of that declaration.
    size_t declaratorCount = 0;
    /// Range that deletes the target declarator from a multi-declarator list, valid only when declaratorCount > 1.
    SourceRange multiRemovalRange;
};

/**
 * @brief Inspects declaration structure at a point to determine unused declarator removal edits.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] line 0-based line of the target point.
 * @param[in] column 0-based column of the target point.
 * @return Declarator removal facts for the enclosing declaration, or empty if not in a declaration.
 * @note Cost: O(depth) descent, ancestor climbs, and a flat cursor loop over declarators.
 */
[[nodiscard]] IndexedDeclaratorRemoval FindUnusedDeclaratorRemoval(const NodeIndex& index, uint32_t line,
                                                                   uint32_t column);

/**
 * @brief 0-based source coordinate point.
 */
struct IndexedSourcePoint
{
    /// 0-based line as used for a Tree-Sitter point.
    uint32_t line = 0;
    /// 0-based column as used for a Tree-Sitter point.
    uint32_t character = 0;
};

/**
 * @brief End point of the opening brace of the class body around a class symbol start.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] classLine 0-based line coordinate of the class symbol start.
 * @param[in] classColumn 0-based column coordinate of the class symbol start.
 * @return nullopt when no class declaration or no opening brace is found, or the index has no tree.
 * @note Cost one descent, a bounded ancestor climb and two flat child walks.
 */
[[nodiscard]] std::optional<IndexedSourcePoint> FindClassOpenBracePoint(const NodeIndex& index, uint32_t classLine,
                                                                        uint32_t classColumn);

} // namespace angel_lsp::analysis
