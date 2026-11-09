#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <optional>
#include <string>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves the type of the expression that spans the given bytes.
 * @param[in] bytes Byte span of the expression.
 * @param[in] ctx Expression context; its cache, type names and node index are used as given.
 * @return Resolved type spelling, or an empty string when no node spans the bytes.
 * @note Cost one root descent, O(depth), plus ResolveExpressionType.
 */
[[nodiscard]] std::string ResolveExpressionType(IndexedByteRange bytes, const ExpressionTypeContext& ctx);

/**
 * @brief Reports the data type an expression names instead of a value.
 * @param[in] bytes Byte span of the expression.
 * @param[in] ctx Expression context.
 * @return The data type name, or nullopt when the expression is a value or no node spans the bytes.
 * @note Cost one root descent, O(depth), plus IsBareDataType.
 */
[[nodiscard]] std::optional<std::string> IsBareDataType(IndexedByteRange bytes, const ExpressionTypeContext& ctx);

/**
 * @brief Reports whether the expression that spans the given bytes names a function.
 * @param[in] bytes Byte span of the expression.
 * @param[in] ctx Expression context.
 * @return True for a function reference; false otherwise or when no node spans the bytes.
 * @note Cost one root descent, O(depth), plus IsFunctionReference.
 */
[[nodiscard]] bool IsFunctionReference(IndexedByteRange bytes, const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
