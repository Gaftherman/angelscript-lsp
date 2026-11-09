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
        site.call = MakeIndexedSourceSpan(callNode);
        site.receiverText = ExtractReceiverText(callNode, source);
    }
    site.args.reserve(argNodes.size());
    for (const TSNode arg : argNodes)
    {
        site.args.push_back(MakeIndexedSourceSpan(arg));
    }
    return site;
}

} // namespace angel_lsp::analysis
