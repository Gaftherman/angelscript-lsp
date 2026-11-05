#include "analysis/index/ReferenceReceiverIndex.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

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
        step.name = parser::GetNodeTextView(parser::GetChildByField(node, grammar.fieldMember), source);
        node = parser::GetChildByField(node, grammar.fieldObject);
    }
    else if (symbol == grammar.symIdentifier || symbol == grammar.symScopedIdentifier ||
             symbol == grammar.symThisExpression)
    {
        step.name = parser::GetNodeTextView(node, source);
        node = {};
    }
    return !step.name.empty();
}

static uint32_t IndexReceiverExpression(TSNode node, std::string_view source,
                                        LocalScopeCollector::DocumentContext& context)
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
static void IndexReferenceQualifier(TSNode parent, std::string_view source, uint32_t referenceStart,
                                    IndexedReferenceReceivers& receivers)
{
    if (receivers.qualifiers.contains(referenceStart))
    {
        return;
    }

    const auto& grammar = parser::GetGrammarSymbols();
    std::string prefix;
    const uint32_t parentLen = ts_node_end_byte(parent) - ts_node_start_byte(parent);
    prefix.reserve(parentLen);
    parser::ForEachChildNode(parent,
                             [&](TSNode child)
                             {
                                 const auto symbol = ts_node_symbol(child);
                                 if (symbol == grammar.symIdentifier && !prefix.empty())
                                 {
                                     const std::string_view qualifier =
                                         prefix == utils::k_scopeSeparator ? std::string_view(prefix)
                                                                           : utils::StripTrailingScopeSeparator(prefix);
                                     receivers.qualifiers.try_emplace(ts_node_start_byte(child), qualifier);
                                 }
                                 if (symbol != grammar.symComment)
                                 {
                                     prefix.append(parser::GetNodeTextView(child, source));
                                 }
                             });
}

void IndexReferenceReceiver(TSNode parent, std::string_view source, LocalReference& reference,
                            LocalScopeCollector::DocumentContext& context)
{
    const auto& grammar = parser::GetGrammarSymbols();
    const auto symbol = ts_node_symbol(parent);
    if (symbol != grammar.symScopedIdentifier && symbol != grammar.symMemberExpression)
    {
        return;
    }

    const bool scoped = symbol == grammar.symScopedIdentifier;
    reference.isScopedIdentifier = scoped;
    reference.qualifierStartByte = ts_node_start_byte(parent);
    reference.qualifierEndByte = ts_node_end_byte(parent);
    const TSNode receiver =
        scoped ? ts_node_named_child(parent, 0) : parser::GetChildByField(parent, grammar.fieldObject);
    if (!ts_node_is_null(receiver))
    {
        reference.receiverStartByte = ts_node_start_byte(receiver);
        reference.receiverEndByte = ts_node_end_byte(receiver);
    }

    if (scoped && reference.qualifierStartByte < reference.startByte)
    {
        IndexReferenceQualifier(parent, source, reference.startByte, *context.receivers);
    }
    else if (!scoped && reference.isMemberAccess)
    {
        const uint32_t id = IndexReceiverExpression(receiver, source, context);
        if (id != UINT32_MAX)
        {
            context.receivers->byReference.try_emplace(reference.startByte, id);
        }
    }
}
} // namespace angel_lsp::analysis
