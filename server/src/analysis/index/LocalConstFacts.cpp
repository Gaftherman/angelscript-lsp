#include "analysis/index/LocalConstFacts.h"

#include "analysis/index/CallArgumentFacts.h"
#include "analysis/index/InlayHintFactsInternal.h"
#include "analysis/index/IsolationIdentifierFacts.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <tuple>

namespace angel_lsp::analysis
{

/** @brief Records identifier storage reached through parentheses, selections, or conditional branches. */
static void RecordStorageUses(TSNode expression, IndexedLocalConstUse use, const NodeIndex& index,
                              std::vector<IndexedLocalConstUse>& uses)
{
    const auto& gs = parser::GetGrammarSymbols();
    std::vector<TSNode> pending{expression};
    while (!pending.empty())
    {
        const TSNode node = UnwrapIndexedParentheses(pending.back(), &index);
        pending.pop_back();
        if (ts_node_is_null(node))
        {
            continue;
        }
        const auto symbol = ts_node_symbol(node);
        if (symbol == gs.symIdentifier || (symbol == gs.symScopedIdentifier && ts_node_named_child_count(node) == 1 &&
                                           ts_node_child_count(node) == 1))
        {
            use.storage = {BytesOf(node), ToSourceRange(node)};
            uses.push_back(use);
        }
        else if (symbol == gs.symMemberExpression || symbol == gs.symIndexExpression)
        {
            pending.push_back(parser::GetChildByField(node, gs.fieldObject));
        }
        else if (symbol == gs.symCastExpression)
        {
            pending.push_back(parser::GetChildByField(node, gs.fieldValue));
        }
        else if (symbol == gs.symTernaryExpression)
        {
            pending.push_back(parser::GetChildByField(node, gs.fieldConsequence));
            pending.push_back(parser::GetChildByField(node, gs.fieldAlternative));
        }
    }
}

/** @brief Captures the callee contract for arguments and the receiver in an invocation. */
static void RecordCallUses(TSNode node, IndexedCallContext context, const NodeIndex& index,
                           std::vector<IndexedLocalConstUse>& uses)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (context.kind != CallContextKind::ConstructorDeclaration)
    {
        const auto target = ExtractCallTargetSpans(node, gs);
        context.calleeStartByte = target.calleeStartByte;
        context.calleeEndByte = target.calleeEndByte;
        context.receiverStartByte = target.receiverStartByte;
        context.receiverEndByte = target.receiverEndByte;
        if (target.isQualified)
        {
            context.kind = CallContextKind::Qualified;
            context.receiverStartByte = target.qualifierStartByte;
            context.receiverEndByte = target.qualifierEndByte;
        }
        else if (target.isMember)
        {
            context.kind = CallContextKind::Member;
            const TSNode receiver = parser::GetChildByField(target.funcNode, gs.fieldObject);
            RecordStorageUses(receiver, {.call = context, .isReceiver = true}, index, uses);
        }
    }

    for (const auto& argument : ExtractCallArguments(node, {}))
    {
        RecordStorageUses(
            argument.exprNode,
            {.call = context, .parameterName = BytesOf(argument.nameNode), .argumentIndex = argument.index}, index,
            uses);
    }
}

/** @brief Captures a single declarator, or retains constructor arguments for reference checks. */
static void RecordDeclaration(TSNode declaration, const NodeIndex& index,
                              std::vector<IndexedLocalConstDeclaration>& declarations,
                              std::vector<IndexedLocalConstUse>& uses)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode type = parser::GetChildByField(declaration, gs.fieldVarType);
    if (ts_node_is_null(type))
    {
        return;
    }

    const auto typeBytes = BytesOf(type);
    TSNode initializedName{};
    size_t declaratorCount = 0;
    TSTreeCursor cursor = ts_tree_cursor_new(declaration);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            const TSNode declarator = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(declarator) != gs.symVariableDeclarator)
            {
                continue;
            }
            ++declaratorCount;
            const TSNode name = parser::GetChildByField(declarator, gs.fieldName);
            const TSNode arguments = parser::GetChildByField(declarator, gs.fieldArguments);
            if (!ts_node_is_null(arguments))
            {
                const auto nameBytes = BytesOf(name);
                RecordCallUses(arguments,
                               {.calleeStartByte = nameBytes.startByte,
                                .calleeEndByte = nameBytes.endByte,
                                .prevTokenStartByte = typeBytes.startByte,
                                .prevTokenEndByte = typeBytes.endByte,
                                .kind = CallContextKind::ConstructorDeclaration},
                               index, uses);
            }
            if (!ts_node_is_null(arguments) || !ts_node_is_null(parser::GetChildByField(declarator, gs.fieldValue)))
            {
                initializedName = name;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);

    if (declaratorCount == 1 && !ts_node_is_null(initializedName) && !ts_node_has_error(declaration) &&
        ts_node_is_null(parser::FindChildWithSymbol(type, gs.tokConst)))
    {
        declarations.push_back({ToSourceRange(type), {BytesOf(initializedName), ToSourceRange(initializedName)}});
    }
}

LocalConstFacts::LocalConstFacts(const NodeIndex& index)
{
    const auto& gs = parser::GetGrammarSymbols();
    const std::array symbols{gs.symVariableDeclaration, gs.symCallExpression, gs.symConstructCallExpression,
                             gs.symUnaryExpression};
    index.ForEachNodeOrdered(symbols,
                             [&](TSNode node)
                             {
                                 const auto symbol = ts_node_symbol(node);
                                 if (symbol == gs.symVariableDeclaration)
                                 {
                                     RecordDeclaration(node, index, m_declarations, m_uses);
                                 }
                                 else if (symbol == gs.symUnaryExpression)
                                 {
                                     const TSNode op = parser::GetChildByField(node, gs.fieldOperator);
                                     if (!ts_node_is_null(op) &&
                                         (ts_node_symbol(op) == gs.tokAt || ts_node_symbol(op) == gs.tokAmp))
                                     {
                                         RecordStorageUses(parser::GetChildByField(node, gs.fieldOperand),
                                                           {.takesAddress = true}, index, m_uses);
                                     }
                                 }
                                 else
                                 {
                                     RecordCallUses(node, {}, index, m_uses);
                                 }
                             });
}

const IndexedLocalConstDeclaration* LocalConstFacts::FindAt(IndexedSourcePoint point) const noexcept
{
    const auto position = std::tie(point.line, point.character);
    const auto past = std::partition_point(m_declarations.begin(), m_declarations.end(),
                                           [&](const IndexedLocalConstDeclaration& declaration)
                                           {
                                               const auto& type = declaration.typeRange;
                                               return std::tie(type.startLine, type.startCharacter) <= position;
                                           });
    if (past == m_declarations.begin())
    {
        return nullptr;
    }
    const auto& declaration = *std::prev(past);
    const auto& name = declaration.name.range;
    const auto& type = declaration.typeRange;
    if (position < std::tie(type.endLine, type.endCharacter) ||
        (std::tie(name.startLine, name.startCharacter) <= position &&
         position < std::tie(name.endLine, name.endCharacter)))
    {
        return &declaration;
    }
    return nullptr;
}

size_t LocalConstFacts::EstimateMemoryBytes() const noexcept
{
    return sizeof(*this) + m_declarations.capacity() * sizeof(IndexedLocalConstDeclaration) +
           m_uses.capacity() * sizeof(IndexedLocalConstUse);
}

} // namespace angel_lsp::analysis
