#include "analysis/checkers/IsolationCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Checks whether any descendant of a node has the given grammar symbol.
 * @param[in] root Subtree root to scan.
 * @param[in] targetSym Grammar symbol to look for.
 * @return True when a descendant of root carries targetSym.
 */
static bool ContainsSymbolDescendant(TSNode root, TSSymbol targetSym) noexcept
{
    if (ts_node_is_null(root))
    {
        return false;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool found = false;
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        uint32_t depth = 1;
        while (depth > 0)
        {
            if (ts_node_symbol(ts_tree_cursor_current_node(&cursor)) == targetSym)
            {
                found = true;
                break;
            }
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                ++depth;
                continue;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                continue;
            }
            while (depth > 0 && !ts_tree_cursor_goto_next_sibling(&cursor))
            {
                ts_tree_cursor_goto_parent(&cursor);
                --depth;
            }
        }
    }
    ts_tree_cursor_delete(&cursor);
    return found;
}

static bool IsAllowedSharedSymbol(TSSymbol declSym, const parser::GrammarSymbols& gs) noexcept
{
    return declSym == gs.symClassDeclaration || declSym == gs.symInterfaceDeclaration ||
           declSym == gs.symFuncdefDeclaration || declSym == gs.symFuncDeclaration;
}

/**
 * @brief Checks that a shared modifier is not followed by an error node holding a primitive type.
 * @param[in] parent Modifier node that owns the shared token.
 * @param[in] gs Grammar symbol table.
 * @return False when the next significant sibling is an error node containing a primitive type; true otherwise.
 */
static bool IsValidEnumSharedModifier(TSNode parent, const parser::GrammarSymbols& gs) noexcept
{
    TSNode next = ts_node_next_sibling(parent);
    while (!ts_node_is_null(next))
    {
        const TSSymbol sym = ts_node_symbol(next);
        if (sym != gs.symSharedExternalModifier && sym != gs.symDeclarationModifier && sym != gs.symComment)
        {
            break;
        }
        next = ts_node_next_sibling(next);
    }

    if (!ts_node_is_null(next) && ts_node_is_error(next))
    {
        return !ContainsSymbolDescendant(next, gs.symPrimitiveType);
    }
    return true;
}

static bool IsValidSharedNode(TSNode sharedNode)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode parent = parser::GetParent(sharedNode);
    if (ts_node_is_null(parent))
    {
        return true;
    }

    TSNode declNode = parent;
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symDeclarationModifier || parentSym == gs.symSharedExternalModifier)
    {
        declNode = parser::GetParent(parent);
    }

    if (ts_node_is_null(declNode) || ts_node_is_error(declNode))
    {
        return false;
    }

    const TSSymbol declSym = ts_node_symbol(declNode);
    if (IsAllowedSharedSymbol(declSym, gs))
    {
        return true;
    }
    if (declSym == gs.symEnumDeclaration)
    {
        return IsValidEnumSharedModifier(parent, gs);
    }
    return false;
}

void CheckSharedEntityEligibility(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (!request.nodeIndex)
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const auto sharedNodes = request.nodeIndex->Nodes(gs.tokShared);

    for (TSNode sharedNode : sharedNodes)
    {
        if (request.nodeIndex->IsInsideComment(ts_node_start_byte(sharedNode)))
        {
            continue;
        }

        if (!IsValidSharedNode(sharedNode))
        {
            const TSPoint start = ts_node_start_point(sharedNode);
            const TSPoint end = ts_node_end_point(sharedNode);
            ctx.LogRule("CheckSharedEntityEligibility", "as-err-shared-not-allowed-on-entity", {});
            ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-shared-not-allowed-on-entity");
        }
    }
}

} // namespace angel_lsp::analysis
