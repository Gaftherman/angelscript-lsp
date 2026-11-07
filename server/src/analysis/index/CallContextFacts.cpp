#include "analysis/index/CallContextFacts.h"

#include "analysis/index/InlayHintFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Pops up to @p count angle openers from the top of the bracket stack.
 *
 * `>>` and `>>>` close several template lists at once. Only `<` entries are removed, so a stray `>` never
 * unbalances parentheses.
 *
 * @param[in,out] openers Stack of opening token symbols seen so far.
 * @param[in] count Maximum number of `<` entries to close.
 * @param[in] gs Precompiled grammar symbols.
 */
static void CloseAngles(std::vector<TSSymbol>& openers, uint32_t count, const parser::GrammarSymbols& gs)
{
    for (uint32_t i = 0; i < count && !openers.empty() && openers.back() == gs.tokLt; ++i)
    {
        openers.pop_back();
    }
}

/**
 * @brief Number of template argument lists closed by a `>`, `>>` or `>>>` token.
 * @param[in] symbol Token symbol to classify.
 * @param[in] gs Precompiled grammar symbols.
 * @return 1, 2 or 3 for the closing angle tokens; 0 for any other symbol.
 */
static uint32_t AngleCloseCount(TSSymbol symbol, const parser::GrammarSymbols& gs) noexcept
{
    if (symbol == gs.tokGt)
    {
        return 1;
    }
    if (symbol == gs.tokGtGt)
    {
        return 2;
    }
    return symbol == gs.tokGtGtGt ? 3 : 0;
}

/**
 * @brief Walks the tokens of a call container up to the cursor and returns the innermost unclosed parenthesis.
 *
 * One flat TSTreeCursor visits the container's children and enters ERROR children. Other subtrees (strings,
 * heredocs, comments, complete calls) are opaque; a `( [ { <` stack ignores commas nested inside brackets.
 *
 * Cost: O(k) in the number of tokens of the container that start before the cursor.
 *
 * @param[in] container argument_list or ERROR node that encloses the cursor.
 * @param[in] cursorByte Cursor byte offset.
 * @param[in] gs Precompiled grammar symbols.
 * @return Callee node written before the unclosed `(` (null when the `(` belongs to the container itself) and the
 *         number of top-level commas after it, or std::nullopt when every parenthesis is closed before the cursor.
 */
static std::optional<std::pair<TSNode, uint32_t>> FindOpenParenthesis(TSNode container, uint32_t cursorByte,
                                                                      const parser::GrammarSymbols& gs)
{
    std::vector<TSSymbol> openers;
    std::vector<std::pair<TSNode, uint32_t>> parens;
    TSNode lastCallee{};
    uint32_t depth = 0;

    TSTreeCursor cursor = ts_tree_cursor_new(container);
    bool hasNode = ts_tree_cursor_goto_first_child(&cursor);
    while (hasNode)
    {
        const TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_start_byte(node) >= cursorByte)
        {
            break;
        }
        if (ts_node_is_error(node) && ts_tree_cursor_goto_first_child(&cursor))
        {
            ++depth;
            continue;
        }

        const TSSymbol symbol = ts_node_symbol(node);
        if (symbol == gs.symIdentifier || symbol == gs.symScopedIdentifier || symbol == gs.symMemberExpression)
        {
            lastCallee = node;
        }
        else if (symbol == gs.tokOpenParen)
        {
            openers.push_back(symbol);
            parens.emplace_back(lastCallee, 0);
            lastCallee = TSNode{};
        }
        else if (symbol == gs.tokOpenBracket || symbol == gs.tokOpenBrace || symbol == gs.tokLt)
        {
            openers.push_back(symbol);
        }
        else if (symbol == gs.tokCloseParen || symbol == gs.tokCloseBracket || symbol == gs.tokCloseBrace)
        {
            const TSSymbol expected = symbol == gs.tokCloseParen     ? gs.tokOpenParen
                                      : symbol == gs.tokCloseBracket ? gs.tokOpenBracket
                                                                     : gs.tokOpenBrace;
            CloseAngles(openers, UINT32_MAX, gs);
            if (!openers.empty() && openers.back() == expected)
            {
                openers.pop_back();
                if (expected == gs.tokOpenParen && !parens.empty())
                {
                    parens.pop_back();
                }
            }
        }
        else if (const uint32_t closes = AngleCloseCount(symbol, gs); closes > 0)
        {
            CloseAngles(openers, closes, gs);
        }
        else if (symbol == gs.symBinaryExpression)
        {
            const TSNode op = parser::GetChildByField(node, gs.fieldOperator);
            CloseAngles(openers, ts_node_is_null(op) ? 0 : AngleCloseCount(ts_node_symbol(op), gs), gs);
        }
        else if (symbol == gs.tokComma && !openers.empty() && openers.back() == gs.tokOpenParen && !parens.empty())
        {
            ++parens.back().second;
        }

        hasNode = ts_tree_cursor_goto_next_sibling(&cursor);
        while (!hasNode && depth > 0 && ts_tree_cursor_goto_parent(&cursor))
        {
            --depth;
            hasNode = ts_tree_cursor_goto_next_sibling(&cursor);
        }
    }
    ts_tree_cursor_delete(&cursor);

    if (parens.empty())
    {
        return std::nullopt;
    }
    return parens.back();
}

/**
 * @brief Builds a call context from the extracted callee and receiver spans of a call target.
 * @param[in] target Spans produced by ExtractTargetSpansFromFunctionNode / ExtractCallTargetSpans.
 * @param[in] activeParameter Number of top-level commas before the cursor.
 * @return Context classified as Member, Qualified or Direct; a qualified callee reports its qualifier through the
 *         receiver span.
 */
static IndexedCallContext MakeCallContext(const CallTargetSpans& target, uint32_t activeParameter) noexcept
{
    IndexedCallContext context;
    context.calleeStartByte = target.calleeStartByte;
    context.calleeEndByte = target.calleeEndByte;
    context.activeParameter = activeParameter;
    if (target.isMember)
    {
        context.kind = CallContextKind::Member;
        context.receiverStartByte = target.receiverStartByte;
        context.receiverEndByte = target.receiverEndByte;
    }
    else if (target.isQualified)
    {
        context.kind = CallContextKind::Qualified;
        context.receiverStartByte = target.qualifierStartByte;
        context.receiverEndByte = target.qualifierEndByte;
    }
    return context;
}

/**
 * @brief Builds a context whose callee is a written type: a construct call or a direct-initialized declarator.
 * @param[in] typeNode Type node naming the constructed class.
 * @param[in] kind Direct for `T(args)` construct calls, ConstructorDeclaration for `T name(args)`.
 * @param[in] activeParameter Number of top-level commas before the cursor.
 * @return Context with the type as callee and, for declarations, also as the preceding token.
 */
static IndexedCallContext MakeTypeContext(TSNode typeNode, CallContextKind kind, uint32_t activeParameter) noexcept
{
    IndexedCallContext context;
    context.calleeStartByte = ts_node_start_byte(typeNode);
    context.calleeEndByte = ts_node_end_byte(typeNode);
    if (kind == CallContextKind::ConstructorDeclaration)
    {
        context.prevTokenStartByte = context.calleeStartByte;
        context.prevTokenEndByte = context.calleeEndByte;
    }
    context.activeParameter = activeParameter;
    context.kind = kind;
    return context;
}

/**
 * @brief Checks whether the cursor lies between an argument list's `(` and its closing `)`.
 * @param[in] list argument_list node.
 * @param[in] cursorByte Cursor byte offset.
 * @param[in] gs Precompiled grammar symbols.
 * @return True when the cursor is after `(` and at or before a written `)`; without a written `)` the list end
 *         bounds the call.
 */
static bool IsInsideArgumentList(TSNode list, uint32_t cursorByte, const parser::GrammarSymbols& gs) noexcept
{
    if (cursorByte <= ts_node_start_byte(list))
    {
        return false;
    }
    const uint32_t count = ts_node_child_count(list);
    const TSNode last = count > 0 ? ts_node_child(list, count - 1) : TSNode{};
    if (!ts_node_is_null(last) && ts_node_symbol(last) == gs.tokCloseParen && !ts_node_is_missing(last))
    {
        return cursorByte <= ts_node_start_byte(last);
    }
    return cursorByte <= ts_node_end_byte(list);
}

/**
 * @brief Resolves the call that owns an argument list enclosing the cursor.
 * @param[in] list Innermost argument_list enclosing the cursor.
 * @param[in] cursorByte Cursor byte offset.
 * @param[in] gs Precompiled grammar symbols.
 * @return Context of the owning call_expression, construct_call_expression or direct-initialized declarator, or of
 *         an unclosed call inside an ERROR child of the list; std::nullopt for any other owner.
 */
static std::optional<IndexedCallContext> ResolveArgumentListCall(TSNode list, uint32_t cursorByte,
                                                                 const parser::GrammarSymbols& gs)
{
    const auto open = FindOpenParenthesis(list, cursorByte, gs);
    if (!open)
    {
        return std::nullopt;
    }
    if (!ts_node_is_null(open->first))
    {
        return MakeCallContext(ExtractTargetSpansFromFunctionNode(open->first, gs), open->second);
    }

    const TSNode owner = parser::GetParent(list);
    const TSSymbol ownerSymbol = ts_node_is_null(owner) ? TSSymbol{0} : ts_node_symbol(owner);
    if (ownerSymbol == gs.symCallExpression)
    {
        const CallTargetSpans target = ExtractCallTargetSpans(owner, gs);
        return ts_node_is_null(target.funcNode) ? std::nullopt : std::optional{MakeCallContext(target, open->second)};
    }
    if (ownerSymbol == gs.symConstructCallExpression)
    {
        const TSNode typeNode = parser::GetChildByField(owner, gs.fieldType);
        return ts_node_is_null(typeNode)
                   ? std::nullopt
                   : std::optional{MakeTypeContext(typeNode, CallContextKind::Direct, open->second)};
    }
    if (ownerSymbol == gs.symVariableDeclarator)
    {
        const TSNode declaration = parser::GetParent(owner);
        const TSNode typeNode =
            (!ts_node_is_null(declaration) && ts_node_symbol(declaration) == gs.symVariableDeclaration)
                ? parser::GetChildByField(declaration, gs.fieldVarType)
                : TSNode{};
        return ts_node_is_null(typeNode)
                   ? std::nullopt
                   : std::optional{MakeTypeContext(typeNode, CallContextKind::ConstructorDeclaration, open->second)};
    }
    return std::nullopt;
}

/**
 * @brief Checks whether no token sits between the end of a node and the cursor.
 *
 * The next token is the next sibling of the node or of its nearest ancestor that has one. The climb is bounded
 * to 16 hops (Rule 4.7); a deeper node is treated as not reaching the cursor.
 *
 * @param[in] node Candidate ERROR node that ends before the cursor.
 * @param[in] cursorByte Cursor byte offset.
 * @return True when the next token starts at or after the cursor (or none follows), so only whitespace separates
 *         the unfinished call from the cursor.
 */
static bool ReachesCursor(TSNode node, uint32_t cursorByte) noexcept
{
    uint32_t hops = 0;
    bool reaches = false;
    parser::ForEachAncestor(node,
                            [&](TSNode ancestor) noexcept -> bool
                            {
                                const TSNode next = ts_node_next_sibling(ancestor);
                                if (!ts_node_is_null(next))
                                {
                                    reaches = ts_node_start_byte(next) >= cursorByte;
                                    return false;
                                }
                                reaches = ts_node_is_null(parser::GetParent(ancestor));
                                return ++hops < 16;
                            });
    return reaches;
}

std::optional<IndexedCallContext> FindCallContext(const NodeIndex& index, uint32_t cursorByte)
{
    const auto& gs = parser::GetGrammarSymbols();

    TSNode innermostList{};
    for (TSNode list : index.Nodes(gs.symArgumentList))
    {
        if (ts_node_start_byte(list) >= cursorByte)
        {
            break;
        }
        if (IsInsideArgumentList(list, cursorByte, gs))
        {
            innermostList = list;
        }
    }
    if (!ts_node_is_null(innermostList))
    {
        if (auto context = ResolveArgumentListCall(innermostList, cursorByte, gs))
        {
            return context;
        }
    }

    TSNode innermostError{};
    for (TSNode error : index.Nodes(gs.symError))
    {
        if (ts_node_start_byte(error) >= cursorByte)
        {
            break;
        }
        if (cursorByte <= ts_node_end_byte(error) || ReachesCursor(error, cursorByte))
        {
            innermostError = error;
        }
    }
    if (ts_node_is_null(innermostError))
    {
        return std::nullopt;
    }
    const auto open = FindOpenParenthesis(innermostError, cursorByte, gs);
    if (!open || ts_node_is_null(open->first))
    {
        return std::nullopt;
    }
    const CallTargetSpans target = ExtractTargetSpansFromFunctionNode(open->first, gs);
    return ts_node_is_null(target.funcNode) ? std::nullopt : std::optional{MakeCallContext(target, open->second)};
}
} // namespace angel_lsp::analysis
