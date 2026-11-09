#include "analysis/index/EngineDialectFacts.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Constructs an indexed source span from a Tree-Sitter AST node.
 * @param[in] node AST node to measure.
 * @return Source coordinates and byte bounds of the node, or empty span if null.
 */
static IndexedSourceSpan MakeSourceSpan(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return IndexedSourceSpan{};
    }
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return IndexedSourceSpan{
        IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)},
        start.row,
        start.column,
        end.row,
        end.column,
    };
}

std::string_view ExtractReceiverText(TSNode callNode, std::string_view sourceCode) noexcept
{
    if (ts_node_is_null(callNode))
    {
        return {};
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode callee = parser::GetChildByField(callNode, gs.fieldFunction);
    if (ts_node_is_null(callee) && ts_node_child_count(callNode) > 0)
    {
        callee = ts_node_child(callNode, 0);
    }
    if (ts_node_is_null(callee))
    {
        return {};
    }
    const TSNode objectNode = parser::GetChildByField(callee, gs.fieldObject);
    return !ts_node_is_null(objectNode) ? parser::NodeText(objectNode, sourceCode) : std::string_view{};
}

SchedulerCallSite BuildSchedulerCallSite(TSNode callNode, std::span<const TSNode> argNodes, std::string_view source)
{
    SchedulerCallSite site;
    if (!ts_node_is_null(callNode))
    {
        site.call = MakeSourceSpan(callNode);
        site.receiverText = ExtractReceiverText(callNode, source);
    }
    site.args.reserve(argNodes.size());
    for (const TSNode arg : argNodes)
    {
        site.args.push_back(MakeSourceSpan(arg));
    }
    return site;
}

} // namespace angel_lsp::analysis
