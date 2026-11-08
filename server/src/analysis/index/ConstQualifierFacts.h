#pragma once

#include "analysis/index/ClassMutationFacts.h"
#include "analysis/index/CodeActionFacts.h"
#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Syntactic call site context for missing const method call diagnostics.
 */
struct IndexedConstCallSite
{
    /// Node at the diagnostic start.
    IndexedByteRange methodName;
    /// Object field of its parent, empty when absent.
    IndexedByteRange receiver;
};

/**
 * @brief Finds the call site identifier and receiver ranges for a missing const diagnostic.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] line 0-based line of the diagnostic start.
 * @param[in] column 0-based column of the diagnostic start.
 * @return Call site byte ranges; empty ranges if the tree or node is null.
 * @note Cost: O(depth) descent and parent lookup.
 */
[[nodiscard]] IndexedConstCallSite FindConstCallSite(const NodeIndex& index, uint32_t line, uint32_t column);

/**
 * @brief Finds the insertion point at the end of a method parameter list.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] line 0-based line coordinate within the target method.
 * @param[in] column 0-based column coordinate within the target method.
 * @return End point coordinates of the parameter list if found; std::nullopt otherwise.
 * @note Cost: O(depth) descent, ancestor climb, and child field lookup.
 */
[[nodiscard]] std::optional<IndexedSourcePoint> FindMethodParameterListEnd(const NodeIndex& index, uint32_t line,
                                                                           uint32_t column);

/**
 * @brief Syntactic context and mutation facts for adding intentional const to a method.
 */
struct IndexedIntentionalConstSite
{
    /// Insertion point at the end of the method parameter list.
    IndexedSourcePoint parameterListEnd;
    /// 0-based start line of the function declaration.
    uint32_t methodStartLine = 0;
    /// Name field of the enclosing class declaration, empty when absent.
    IndexedByteRange className;
    /// Pre-indexed mutation facts and candidate sites for the method body.
    IndexedMethodMutationFacts mutation;
};

/**
 * @brief Finds the intentional const qualification context for a method at the cursor position.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] line 0-based line of the cursor position.
 * @param[in] column 0-based column of the cursor position.
 * @return Intentional const site facts if eligible; std::nullopt otherwise.
 * @note Cost: O(depth) descent, ancestor climbs, and flat body cursor walk.
 */
[[nodiscard]] std::optional<IndexedIntentionalConstSite>
FindIntentionalConstSite(const NodeIndex& index, std::string_view sourceCode, uint32_t line, uint32_t column);

/**
 * @brief Syntactic context for adding a const qualifier to a handle variable declaration.
 */
struct IndexedConstHandleSite
{
    /// Byte range of the variable type specifier node.
    IndexedByteRange typeText;
    /// Source coordinates of the start of the variable type specifier.
    IndexedSourcePoint typeStart;
};

/**
 * @brief Finds the variable type specifier site for adding a const qualifier.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] line 0-based line of the diagnostic position.
 * @param[in] column 0-based column of the diagnostic position.
 * @return Type specifier range and start point if located; std::nullopt otherwise.
 * @note Cost: O(depth) descent, ancestor climb, and child field lookup.
 */
[[nodiscard]] std::optional<IndexedConstHandleSite> FindConstHandleSite(const NodeIndex& index, uint32_t line,
                                                                        uint32_t column);

} // namespace angel_lsp::analysis
