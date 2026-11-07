#include "analysis/index/HoverFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <array>
#include <cstddef>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
ContainerContextPreference CheckContextPreference(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (IsScopeQualifier(node))
    {
        TSNode usingDecl = parser::FindAncestorWithSymbol(parser::GetParent(node), gs.symUsingDeclaration);
        return !ts_node_is_null(usingDecl) ? ContainerContextPreference::PreferNamespace
                                           : ContainerContextPreference::None;
    }

    ContainerContextPreference pref = ContainerContextPreference::None;
    parser::ForEachAncestor(parser::GetParent(node),
                            [&](TSNode cur) -> bool
                            {
                                const TSSymbol sym = ts_node_symbol(cur);
                                if (sym == gs.symDatatype || sym == gs.symTemplateTypeList ||
                                    sym == gs.symBaseClassList || sym == gs.symType)
                                {
                                    pref = ContainerContextPreference::PreferClass;
                                    return false;
                                }
                                if (sym == gs.symUsingDeclaration)
                                {
                                    pref = ContainerContextPreference::PreferNamespace;
                                    return false;
                                }
                                return sym == gs.symScopedIdentifier || sym == gs.symScope;
                            });
    return pref;
}

bool IsScopeQualifier(TSNode node) noexcept
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symScope)
    {
        return true;
    }
    if (parentSym != gs.symScopedIdentifier)
    {
        return false;
    }
    size_t siblingHops = 0;
    TSNode next = ts_node_next_sibling(node);
    while (!ts_node_is_null(next) && siblingHops++ < 8)
    {
        if (ts_node_symbol(next) == gs.tokColonColon)
        {
            return true;
        }
        next = ts_node_next_sibling(next);
    }
    return false;
}

TSNode FindNextScopedMemberNode(TSNode parent, TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symScopedIdentifier)
    {
        return TSNode{};
    }
    TSTreeCursor cursor = ts_tree_cursor_new(parent);
    bool foundCurrent = false;
    TSNode nextMember{};
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode current = ts_tree_cursor_current_node(&cursor);
            if (foundCurrent && ts_node_is_named(current))
            {
                nextMember = current;
                break;
            }
            if (!foundCurrent && ts_node_is_named(current) &&
                (ts_node_eq(current, node) || ts_node_start_byte(current) == ts_node_start_byte(node)))
            {
                foundCurrent = true;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return nextMember;
}

bool IsMemberChildOfExpression(TSNode node, TSNode parent)
{
    if (ts_node_is_null(parent))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(parent) != gs.symMemberExpression)
    {
        return false;
    }
    TSNode memNode = parser::GetChildByField(parent, gs.fieldMember);
    return !ts_node_is_null(memNode) &&
           (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node));
}

bool IsNamedArgumentLabelAtCursor(TSNode node, TSNode parent)
{
    if (ts_node_is_null(node) || ts_node_is_null(parent) ||
        ts_node_symbol(parent) != parser::GetGrammarSymbols().symArgumentList)
    {
        return false;
    }
    TSNode next = ts_node_next_sibling(node);
    while (!ts_node_is_null(next) && ts_node_symbol(next) == parser::GetGrammarSymbols().symComment)
    {
        next = ts_node_next_sibling(next);
    }
    return !ts_node_is_null(next) && ts_node_symbol(next) == parser::GetGrammarSymbols().tokColon;
}

TSNode FindEnclosingLambda(TSNode node)
{
    return parser::FindAncestorWithSymbol(node, parser::GetGrammarSymbols().symLambdaExpression);
}

TSNode FindEnclosingAutoDeclaration(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> declSyms = {gs.symVariableDeclaration, gs.symForeachStatement};
    return parser::FindAncestorWithAnySymbol(node, declSyms);
}

TSNode FindTopmostBinaryPlus(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode current = node;
    parser::ForEachAncestor(parser::GetParent(node),
                            [&](TSNode parent) -> bool
                            {
                                if (ts_node_symbol(parent) == gs.symBinaryExpression)
                                {
                                    TSNode opNode = parser::GetChildByField(parent, gs.fieldOperator);
                                    if (!ts_node_is_null(opNode) && ts_node_symbol(opNode) == gs.tokPlus)
                                    {
                                        current = parent;
                                        return true;
                                    }
                                }
                                return false;
                            });
    return current;
}

bool IsQualifiedMemberNode(TSNode node)
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol pSym = ts_node_symbol(parent);
    if (pSym == gs.symScopedIdentifier)
    {
        TSNode firstChild = ts_node_named_child(parent, 0);
        return !ts_node_is_null(firstChild) && !ts_node_eq(firstChild, node) &&
               ts_node_start_byte(firstChild) != ts_node_start_byte(node);
    }
    if (pSym == gs.symMemberExpression)
    {
        TSNode memberField = parser::GetChildByField(parent, gs.fieldMember);
        return !ts_node_is_null(memberField) &&
               (ts_node_eq(memberField, node) || ts_node_start_byte(memberField) == ts_node_start_byte(node));
    }
    if (pSym == gs.symDatatype)
    {
        return !ts_node_is_null(FindDatatypeScopeNode(parent));
    }
    return false;
}

TSNode FindDatatypeScopeNode(TSNode datatypeNode)
{
    TSNode typeNode = parser::GetParent(datatypeNode);
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(typeNode) || ts_node_symbol(typeNode) != gs.symType)
    {
        return TSNode{};
    }
    TSNode scope = parser::FindFirstChildOfSymbol(typeNode, gs.symScope);
    if (ts_node_is_null(scope) || ts_node_start_byte(scope) >= ts_node_end_byte(scope))
    {
        return TSNode{};
    }
    return scope;
}
} // namespace angel_lsp::analysis
