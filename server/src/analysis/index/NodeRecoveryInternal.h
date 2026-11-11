#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Recovers an AST node matching an exact byte range [startByte, endByte) from a root TSNode.
 * @param[in] root Root AST node to search.
 * @param[in] startByte Inclusive start byte offset.
 * @param[in] endByte Exclusive end byte offset.
 * @return Recovered AST node, or null node if recovery fails.
 */
TSNode RecoverNodeFromRoot(TSNode root, uint32_t startByte, uint32_t endByte) noexcept;

/**
 * @brief Recovers an AST node matching an exact byte range [startByte, endByte) from a NodeIndex.
 * @param[in] index Document NodeIndex retaining AST root.
 * @param[in] startByte Inclusive start byte offset.
 * @param[in] endByte Exclusive end byte offset.
 * @return Recovered AST node, or null node if recovery fails.
 */
TSNode RecoverNodeAt(const NodeIndex& index, uint32_t startByte, uint32_t endByte) noexcept;

} // namespace angel_lsp::analysis
