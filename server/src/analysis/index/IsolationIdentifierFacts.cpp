#include "analysis/index/IsolationIdentifierFacts.h"

#include "analysis/index/IsolationSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "utils/Utils.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Returns true when an AST symbol participates in identifier isolation context or stop boundaries.
 * @param[in] sym Tree-sitter grammar symbol identifier of the visited node.
 * @param[in] gs Cached grammar symbol table.
 * @return True if the ancestor node must be pushed onto the context stack.
 */
static bool NeedsAncestorContext(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.symCallExpression || sym == gs.symDatatype || sym == gs.symType || sym == gs.symBaseClassList ||
           sym == gs.symVariableDeclarator || sym == gs.symFuncDeclaration || sym == gs.symClassDeclaration ||
           sym == gs.symParameter || sym == gs.symEnumMember || sym == gs.symMemberExpression ||
           sym == gs.symStatementBlock;
}

/**
 * @brief Captures field targets and byte boundaries once when entering a relevant ancestor node.
 * @param[in] node Ancestor AST node entered by the tree cursor.
 * @param[in] sym Grammar symbol identifier of node.
 * @param[in] depth Tree depth at which node resides.
 * @param[in] gs Cached grammar symbol table.
 * @return Populated IsolationAncestorFrame ready to push onto the ancestor context stack.
 */
static IsolationAncestorFrame MakeAncestorFrame(TSNode node, TSSymbol sym, uint32_t depth,
                                                const parser::GrammarSymbols& gs) noexcept
{
    IsolationAncestorFrame frame{};
    frame.depth = depth;
    frame.sym = sym;

    if (sym == gs.symCallExpression)
    {
        const TSNode fn = parser::GetCallCallee(node);
        frame.targetId = ts_node_is_null(fn) ? nullptr : fn.id;
    }
    else if (sym == gs.symVariableDeclarator)
    {
        const TSNode val = parser::GetChildByField(node, gs.fieldValue);
        frame.hasBoundary = !ts_node_is_null(val);
        frame.boundaryByte = frame.hasBoundary ? ts_node_start_byte(val) : 0;
    }
    else if (sym == gs.symFuncDeclaration || sym == gs.symClassDeclaration || sym == gs.symParameter ||
             sym == gs.symEnumMember)
    {
        const TSNode nameNode = parser::GetChildByField(node, gs.fieldName);
        if (!ts_node_is_null(nameNode))
        {
            frame.targetId = nameNode.id;
            frame.boundaryByte = ts_node_start_byte(nameNode);
        }
    }
    else if (sym == gs.symMemberExpression)
    {
        const TSNode prop = parser::GetChildByField(node, gs.fieldMember);
        frame.targetId = ts_node_is_null(prop) ? nullptr : prop.id;
    }

    return frame;
}

/**
 * @brief Evaluates identifier role flags against the active ancestor stack from innermost to outermost.
 * @param[in] node Identifier AST node being classified.
 * @param[in] parentId Opaque node pointer of node's immediate AST parent, or nullptr at root.
 * @param[in] stack Active ancestor frames ordered from outermost to innermost.
 * @param[in,out] fact Identifier site fact whose role flags are updated.
 */
static void ApplyAncestorContext(TSNode node, const void* parentId, const std::vector<IsolationAncestorFrame>& stack,
                                 IsolationIdentifierSiteFact& fact) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const uint32_t nodeStartByte = ts_node_start_byte(node);

    for (size_t i = stack.size(); i > 0; --i)
    {
        const IsolationAncestorFrame& cur = stack[i - 1];
        const TSSymbol sym = cur.sym;

        if (sym == gs.symCallExpression)
        {
            if (cur.targetId != nullptr && (cur.targetId == node.id || parentId == cur.targetId))
            {
                fact.isSpecialContext = fact.isCallee = true;
                return;
            }
        }
        if (sym == gs.symDatatype || sym == gs.symType || sym == gs.symBaseClassList)
        {
            fact.isSpecialContext = true;
            return;
        }
        if (sym == gs.symVariableDeclarator)
        {
            if (!cur.hasBoundary || nodeStartByte < cur.boundaryByte)
            {
                fact.isSpecialContext = fact.isDeclarationName = true;
                return;
            }
        }
        if (sym == gs.symFuncDeclaration || sym == gs.symClassDeclaration || sym == gs.symParameter ||
            sym == gs.symEnumMember)
        {
            if (cur.targetId != nullptr && (cur.targetId == node.id || nodeStartByte == cur.boundaryByte))
            {
                fact.isSpecialContext = fact.isDeclarationName = true;
                return;
            }
        }
        if (sym == gs.symMemberExpression)
        {
            if (cur.targetId != nullptr && cur.targetId == node.id)
            {
                fact.isSpecialContext = fact.isMember = true;
                return;
            }
        }
        if (sym == gs.symStatementBlock || sym == gs.symFuncDeclaration || sym == gs.symClassDeclaration)
        {
            return;
        }
    }
}

void CollectIsolationIdentifierFacts(const NodeIndex& index, std::string_view source,
                                     std::vector<IsolationIdentifierSiteFact>& identifiers)
{
    const TSNode root = index.Root();
    if (ts_node_is_null(root))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    std::vector<const void*> parentIds;
    parentIds.reserve(64);
    std::vector<IsolationAncestorFrame> contextStack;
    contextStack.reserve(32);

    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool running = true;
    while (running)
    {
        const TSNode node = ts_tree_cursor_current_node(&cursor);
        const TSSymbol sym = ts_node_symbol(node);

        if (sym == gs.symIdentifier && ts_node_is_named(node))
        {
            const uint32_t start = ts_node_start_byte(node);
            if (!index.IsInsideComment(start))
            {
                const auto [cls, sh] = EnclosingContext(index, start, source);
                IsolationIdentifierSiteFact fact{
                    BytesOf(node), RangeOf(node), utils::TrimView(parser::NodeText(node, source)),
                    cls,           false,         false,
                    false,         false,         sh};
                const void* parentId = parentIds.empty() ? nullptr : parentIds.back();
                ApplyAncestorContext(node, parentId, contextStack, fact);
                identifiers.push_back(fact);
            }
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            const uint32_t nodeDepth = static_cast<uint32_t>(parentIds.size());
            parentIds.push_back(node.id);
            if (NeedsAncestorContext(sym, gs))
            {
                contextStack.push_back(MakeAncestorFrame(node, sym, nodeDepth, gs));
            }
            continue;
        }

        while (true)
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                break;
            }
            if (!ts_tree_cursor_goto_parent(&cursor))
            {
                running = false;
                break;
            }
            parentIds.pop_back();
            const uint32_t poppedDepth = static_cast<uint32_t>(parentIds.size());
            if (!contextStack.empty() && contextStack.back().depth == poppedDepth)
            {
                contextStack.pop_back();
            }
        }
    }

    ts_tree_cursor_delete(&cursor);
}

} // namespace angel_lsp::analysis
