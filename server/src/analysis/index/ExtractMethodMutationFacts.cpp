#include "analysis/index/CodeActionFactsInternal.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

static void InsertIfIdentifier(TSNode node, std::string_view sourceCode, std::vector<IndexedByteRange>& mutatedNames)
{
    if (ts_node_is_null(node))
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol t = ts_node_symbol(node);
    if (t == gs.symIdentifier || t == gs.symScopedIdentifier)
    {
        if (!parser::NodeText(node, sourceCode).empty())
        {
            mutatedNames.push_back(IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)});
        }
    }
}

static void CheckAssignmentMutation(TSNode curr, std::string_view sourceCode,
                                    std::vector<IndexedByteRange>& mutatedNames)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode left = parser::GetChildByField(curr, gs.fieldLeft);
    InsertIfIdentifier(left, sourceCode, mutatedNames);
}

static void CheckIncDecMutation(TSNode curr, std::string_view sourceCode, std::vector<IndexedByteRange>& mutatedNames)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(curr, gs.fieldOperator);
    const TSSymbol opSym = ts_node_symbol(opNode);
    if (opSym == gs.tokPlusPlus || opSym == gs.tokDashDash)
    {
        TSNode arg = parser::GetChildByField(curr, gs.fieldOperand);
        InsertIfIdentifier(arg, sourceCode, mutatedNames);
    }
}

/**
 * @brief Identifies arguments passed to a call expression that may be mutated by reference.
 * @param[in] curr Call expression AST node being evaluated.
 * @param[in] sourceCode Document source text buffer.
 * @param[in,out] mutatedNames Output vector accumulating identifiers mutated by the call.
 */
static void CheckCallMutation(TSNode curr, std::string_view sourceCode, std::vector<IndexedByteRange>& mutatedNames)
{
    TSNode argsNode = parser::GetChildByField(curr, parser::GetGrammarSymbols().fieldArguments);
    if (ts_node_is_null(argsNode))
    {
        return;
    }
    uint32_t argCnt = ts_node_named_child_count(argsNode);
    for (uint32_t i = 0; i < argCnt; ++i)
    {
        TSNode arg = ts_node_named_child(argsNode, i);
        std::string_view argText = parser::NodeText(arg, sourceCode);
        if (analysis::IsOutOrInOutArgumentView(argText))
        {
            TSNode idNode = parser::GetChildByField(arg, parser::GetGrammarSymbols().fieldName);
            if (ts_node_is_null(idNode) && ts_node_named_child_count(arg) > 0)
            {
                idNode = ts_node_named_child(arg, ts_node_named_child_count(arg) - 1);
            }
            InsertIfIdentifier(idNode, sourceCode, mutatedNames);
        }
    }
}

static void CheckNodeMutations(TSNode curr, std::string_view sourceCode, std::vector<IndexedByteRange>& mutatedNames)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol type = ts_node_symbol(curr);
    if (type == gs.symAssignmentExpression)
    {
        CheckAssignmentMutation(curr, sourceCode, mutatedNames);
    }
    else if (type == gs.symPostfixExpression || type == gs.symUnaryExpression)
    {
        CheckIncDecMutation(curr, sourceCode, mutatedNames);
    }
    else if (type == gs.symCallExpression)
    {
        CheckCallMutation(curr, sourceCode, mutatedNames);
    }
}

/**
 * @brief Traverses a statement subtree iteratively to detect variables that undergo mutation.
 * @param[in] root Root AST node of the statement subtree to inspect.
 * @param[in] sourceCode Document source text buffer.
 * @param[in,out] mutatedNames Output vector accumulating byte ranges of mutated identifier names.
 */
static void TraverseSubtreeMutations(TSNode root, std::string_view sourceCode,
                                     std::vector<IndexedByteRange>& mutatedNames)
{
    if (ts_node_is_null(root))
    {
        return;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool visiting = true;
    while (visiting)
    {
        const TSNode current = ts_tree_cursor_current_node(&cursor);
        CheckNodeMutations(current, sourceCode, mutatedNames);

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }

        bool backtracked = false;
        while (ts_tree_cursor_goto_parent(&cursor))
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                backtracked = true;
                break;
            }
        }
        if (!backtracked)
        {
            visiting = false;
        }
    }
    ts_tree_cursor_delete(&cursor);
}

void CollectMutatedNameRanges(const std::vector<TSNode>& stmts, std::string_view sourceCode,
                              std::vector<IndexedByteRange>& mutatedNames)
{
    for (const auto& stmt : stmts)
    {
        TraverseSubtreeMutations(stmt, sourceCode, mutatedNames);
    }
}

} // namespace angel_lsp::analysis
