#include "analysis/index/AssignmentSiteFacts.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ValueExpressionFacts.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Converts a node's start and end points into a line/column source range.
 * @param[in] node Node to measure; a null node yields an empty range.
 * @return Zero-based range covering the node's text.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return {start.row, start.column, end.row, end.column};
}

/**
 * @brief Converts a node to its byte span.
 * @param[in] node Syntax node.
 * @return Start and end byte of the node, or a zero range if null.
 */
static IndexedByteRange BytesOf(TSNode node) noexcept
{
    return ts_node_is_null(node) ? IndexedByteRange{}
                                 : IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)};
}

static TSNode UnwrapParens(TSNode node) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(node) && ts_node_symbol(node) == gs.symParenthesizedExpression)
    {
        const TSNode child = ts_node_named_child(node, 0);
        if (ts_node_is_null(child))
        {
            break;
        }
        node = child;
    }
    return node;
}

CallArgumentFact ReadCallArgument(TSNode exprNode, TSNode nameNode, const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    CallArgumentFact fact;
    fact.bytes = BytesOf(exprNode);
    fact.range = RangeOf(exprNode);
    fact.text = parser::NodeText(exprNode, source);
    if (!ts_node_is_null(nameNode))
    {
        fact.isNamed = true;
        fact.name = parser::NodeText(nameNode, source);
        fact.nameRange = RangeOf(nameNode);
    }
    fact.operand = BuildBinaryOperandFact(exprNode, index, source);
    fact.value = ReadValueExpression(exprNode, &index, source);

    const TSNode unwrapped = UnwrapParens(exprNode);
    const TSSymbol sym = ts_node_is_null(unwrapped) ? 0 : ts_node_symbol(unwrapped);
    if (sym == gs.symLambdaExpression)
    {
        fact.kind = CallArgumentKind::Lambda;
        fact.funcdefValue = ReadFuncdefValue(unwrapped, index, source);
    }
    else if (sym == gs.symInitializerList)
    {
        fact.kind = CallArgumentKind::InitializerList;
    }
    else if (sym == gs.symTernaryExpression)
    {
        fact.kind = CallArgumentKind::Ternary;
        fact.isTernary = true;
        const TSNode consequence = parser::GetChildByField(unwrapped, gs.fieldConsequence);
        const TSNode alternative = parser::GetChildByField(unwrapped, gs.fieldAlternative);
        fact.ternaryConsequence = BuildBinaryOperandFact(consequence, index, source);
        fact.ternaryAlternative = BuildBinaryOperandFact(alternative, index, source);
    }
    else if (sym == gs.symMemberExpression)
    {
        fact.kind = CallArgumentKind::Member;
        const TSNode obj = parser::GetChildByField(unwrapped, gs.fieldObject);
        const TSNode mem = parser::GetChildByField(unwrapped, gs.fieldMember);
        if (!ts_node_is_null(obj) && !ts_node_is_null(mem) && parser::NodeText(obj, source) == "this")
        {
            fact.isThisMemberAccess = true;
            fact.thisMemberName = parser::NodeText(mem, source);
        }
    }
    else if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        fact.kind = CallArgumentKind::Identifier;
    }
    else if (sym == gs.symNullLiteral || sym == gs.symNumberLiteral || sym == gs.symStringLiteral ||
             sym == gs.tokTrue || sym == gs.tokFalse)
    {
        fact.kind = CallArgumentKind::Literal;
    }
    return fact;
}

void ExtractCallArguments(TSNode argListNode, const NodeIndex& index, std::string_view source, CallSiteFacts& facts)
{
    if (ts_node_is_null(argListNode))
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSTreeCursor cursor = ts_tree_cursor_new(argListNode);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return;
    }
    TSNode exprNode{};
    TSNode nameNode{};
    auto flush = [&]()
    {
        if (!ts_node_is_null(exprNode))
        {
            facts.arguments.push_back(ReadCallArgument(exprNode, nameNode, index, source));
            exprNode = TSNode{};
            nameNode = TSNode{};
        }
    };
    do
    {
        const TSNode child = ts_tree_cursor_current_node(&cursor);
        const TSSymbol sym = ts_node_symbol(child);
        if (sym == gs.tokOpenParen || sym == gs.symComment || sym == gs.tokColon)
        {
            continue;
        }
        if (sym == gs.tokCloseParen)
        {
            break;
        }
        if (sym == gs.tokComma)
        {
            flush();
            continue;
        }
        if (ts_tree_cursor_current_field_id(&cursor) == gs.fieldArgName)
        {
            nameNode = child;
        }
        else
        {
            if (!ts_node_is_null(exprNode))
            {
                flush();
            }
            exprNode = child;
        }
    } while (ts_tree_cursor_goto_next_sibling(&cursor));
    ts_tree_cursor_delete(&cursor);
    flush();
}

} // namespace angel_lsp::analysis
