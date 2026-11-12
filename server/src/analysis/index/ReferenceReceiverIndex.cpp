#include "analysis/index/ReferenceReceiverIndex.h"

#include "analysis/index/CallArgumentFacts.h"
#include "analysis/index/InlayHintFactsInternal.h"
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
                                     prefix.append(parser::NodeText(child, source));
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

TSNode ScopedIdentifierTail(TSNode scoped) noexcept
{
    const TSNode nameNode = parser::GetChildByField(scoped, parser::GetGrammarSymbols().fieldName);
    if (!ts_node_is_null(nameNode))
    {
        return nameNode;
    }
    const uint32_t count = ts_node_named_child_count(scoped);
    return count > 0 ? ts_node_named_child(scoped, count - 1) : TSNode{};
}

void IndexCallArguments(TSNode callNode, LocalScopeCollector::DocumentContext& context)
{
    if (ts_node_is_null(callNode) || !context.receivers)
    {
        return;
    }

    const auto& grammar = parser::GetGrammarSymbols();
    const auto target = ExtractCallTargetSpans(callNode, grammar);
    if (ts_node_is_null(target.funcNode))
    {
        return;
    }

    TSNode argsChild = parser::GetChildByField(callNode, grammar.fieldArguments);
    if (ts_node_is_null(argsChild))
    {
        TSTreeCursor searchCursor = ts_tree_cursor_new(callNode);
        if (ts_tree_cursor_goto_first_child(&searchCursor))
        {
            do
            {
                TSNode c = ts_tree_cursor_current_node(&searchCursor);
                if (ts_node_symbol(c) == grammar.symArgumentList)
                {
                    argsChild = c;
                    break;
                }
            } while (ts_tree_cursor_goto_next_sibling(&searchCursor));
        }
        ts_tree_cursor_delete(&searchCursor);
    }
    if (ts_node_is_null(argsChild))
    {
        return;
    }

    const auto callArgs = ExtractCallArguments(argsChild, "");
    for (const auto& a : callArgs)
    {
        if (ts_node_is_null(a.exprNode))
        {
            continue;
        }
        IndexedCallArgumentRef argRef{
            .argStartByte = ts_node_start_byte(a.exprNode),
            .argEndByte = ts_node_end_byte(a.exprNode),
            .calleeStartByte = target.calleeStartByte,
            .calleeEndByte = target.calleeEndByte,
            .receiverStartByte = target.receiverStartByte,
            .receiverEndByte = target.receiverEndByte,
            .argumentIndex = a.index,
        };
        context.receivers->callArguments.push_back(argRef);
    }
}
} // namespace angel_lsp::analysis
