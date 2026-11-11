#include "analysis/index/NodeRecoveryInternal.h"

#include "parser/ASTUtils.h"

#include <cstdint>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

TSNode RecoverNodeFromRoot(TSNode root, uint32_t startByte, uint32_t endByte) noexcept
{
    if (ts_node_is_null(root))
    {
        return TSNode{};
    }
    TSNode node = ts_node_named_descendant_for_byte_range(root, startByte, endByte);
    if (ts_node_is_null(node))
    {
        node = ts_node_descendant_for_byte_range(root, startByte, endByte);
    }
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }
    TSNode match = TSNode{};
    parser::ForEachAncestor(node,
                            [&](TSNode ancestor)
                            {
                                if (ts_node_start_byte(ancestor) == startByte && ts_node_end_byte(ancestor) == endByte)
                                {
                                    if (ts_node_is_named(ancestor) || ts_node_is_null(match))
                                    {
                                        match = ancestor;
                                    }
                                    return true;
                                }
                                if (ts_node_start_byte(ancestor) < startByte || ts_node_end_byte(ancestor) > endByte)
                                {
                                    return false;
                                }
                                return true;
                            });
    return !ts_node_is_null(match) ? match : node;
}

TSNode RecoverNodeAt(const NodeIndex& index, uint32_t startByte, uint32_t endByte) noexcept
{
    return RecoverNodeFromRoot(index.Root(), startByte, endByte);
}

} // namespace angel_lsp::analysis
