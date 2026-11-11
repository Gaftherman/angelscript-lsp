#pragma once

#include "analysis/index/NodeRecoveryInternal.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <cstdint>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Internal legacy bridge recovering an AST node matching [startByte, endByte) from context.
 * @param[in] ctx Semantic context containing NodeIndex or rootNode.
 * @param[in] startByte Inclusive start byte offset.
 * @param[in] endByte Exclusive end byte offset.
 * @return Recovered AST node, or null node if recovery fails.
 */
inline TSNode RecoverNodeAt(const ExpressionTypeContext& ctx, uint32_t startByte, uint32_t endByte) noexcept
{
    if (ctx.nodeIndex)
    {
        return RecoverNodeAt(*ctx.nodeIndex, startByte, endByte);
    }
    if (!ts_node_is_null(ctx.rootNode))
    {
        return RecoverNodeFromRoot(ctx.rootNode, startByte, endByte);
    }
    return TSNode{};
}

} // namespace angel_lsp::analysis
