#pragma once

#include "analysis/index/CodeActionFacts.h"
#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Syntactic context for generating accessors for member variable declarations.
 */
struct IndexedAccessorSite
{
    /// Name field of the enclosing class declaration (empty when absent).
    IndexedByteRange className;
    /// Closing brace point of the class body.
    IndexedSourcePoint insertPoint;
    /// Var_type field of the variable declaration, else its type field; empty when absent.
    IndexedByteRange fieldType;
    /// Name field of each variable_declarator child whose text is non-empty, in order.
    std::vector<IndexedByteRange> fieldNames;
};

/**
 * @brief Finds the accessor generation site context at a given document coordinate.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] line 0-based line of the target point.
 * @param[in] column 0-based column of the target point.
 * @return Accessor site if cursor is on a member variable declaration; std::nullopt otherwise.
 * @note Cost: O(depth) descent, ancestor climbs, and a flat cursor loop over declarators.
 */
[[nodiscard]] std::optional<IndexedAccessorSite> FindAccessorSite(const NodeIndex& index, std::string_view sourceCode,
                                                                  uint32_t line, uint32_t column);

/**
 * @brief Finds the insertion point for the 'property' keyword in an accessor function declaration.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] line 0-based line of the function declaration.
 * @param[in] column 0-based column of the function declaration.
 * @return Insertion point coordinates if located; std::nullopt otherwise.
 * @note Cost: O(depth) enclosing node lookup and a flat child walk.
 */
[[nodiscard]] std::optional<IndexedSourcePoint> FindPropertyKeywordInsertionPoint(const NodeIndex& index, uint32_t line,
                                                                                  uint32_t column);

} // namespace angel_lsp::analysis
