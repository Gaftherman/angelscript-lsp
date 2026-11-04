#include "analysis/index/ReferenceReceiverIndex.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <utility>

namespace angel_lsp::analysis
{
static TSNode UnwrapReceiver(TSNode node, const parser::GrammarSymbols& grammar)
{
    while (!ts_node_is_null(node) && ts_node_symbol(node) == grammar.symParenthesizedExpression)
    {
        node = ts_node_named_child(node, 0);
    }
    return node;
}

static bool CaptureStep(TSNode& node, IndexedReceiverExpression& step, std::string_view source)
{
    const auto& grammar = parser::GetGrammarSymbols();
    step.startByte = ts_node_start_byte(node);
    if (ts_node_symbol(node) == grammar.symCallExpression)
    {
        const auto arguments = parser::GetChildByField(node, grammar.fieldArguments);
        step.argumentCount = ts_node_is_null(arguments) ? 0 : ts_node_named_child_count(arguments);
        node = UnwrapReceiver(parser::GetChildByField(node, grammar.fieldFunction), grammar);
    }
    if (ts_node_is_null(node))
    {
        return false;
    }

    const TSSymbol symbol = ts_node_symbol(node);
    if (symbol == grammar.symMemberExpression)
    {
        step.name = parser::NodeText(parser::GetChildByField(node, grammar.fieldMember), source);
        node = parser::GetChildByField(node, grammar.fieldObject);
    }
    else if (symbol == grammar.symIdentifier || symbol == grammar.symScopedIdentifier ||
             symbol == grammar.symThisExpression)
    {
        step.name = parser::NodeText(node, source);
        node = {};
    }
    return !step.name.empty();
}

uint32_t IndexReceiverExpression(TSNode node, std::string_view source, LocalScopeCollector::DocumentContext& context)
{
    const auto& grammar = parser::GetGrammarSymbols();
    std::vector<std::pair<uint64_t, IndexedReceiverExpression>> pending;
    uint32_t base = UINT32_MAX;
    node = UnwrapReceiver(node, grammar);
    while (!ts_node_is_null(node))
    {
        const uint64_t key = (static_cast<uint64_t>(ts_node_start_byte(node)) << 32) | ts_node_end_byte(node);
        if (const auto found = context.receiverIds.find(key); found != context.receiverIds.end())
        {
            base = found->second;
            break;
        }

        IndexedReceiverExpression step;
        if (!CaptureStep(node, step, source))
        {
            return UINT32_MAX;
        }
        pending.push_back({key, std::move(step)});
        node = UnwrapReceiver(node, grammar);
    }

    for (auto it = pending.rbegin(); it != pending.rend(); ++it)
    {
        auto& [key, expression] = *it;
        expression.base = base;
        base = static_cast<uint32_t>(context.receivers->expressions.size());
        context.receivers->expressions.push_back(std::move(expression));
        context.receiverIds.try_emplace(key, base);
    }
    return base;
}
} // namespace angel_lsp::analysis
