#pragma once

#include "analysis/index/NodeIndex.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief One call expression with the string value of each of its arguments, for string-reflection lookups.
 */
struct IndexedReflectionCall
{
    /// Text of the called expression (function name, possibly scoped or member-qualified); borrows the document source.
    std::string_view calleeText{};
    /// Per argument, in call order: the string literals found in it, unquoted and joined across binary expressions.
    /// Empty for an argument that holds no string literal.
    std::vector<std::string> argStringValues{};
};

/**
 * @brief Collects every call expression of the document together with the literal text of its arguments.
 * @param[in] index First-pass index of the document.
 * @param[in] source Document text that the index was built from.
 * @return One entry per call that has a function node, in document order.
 */
std::vector<IndexedReflectionCall> BuildReflectionCallFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
