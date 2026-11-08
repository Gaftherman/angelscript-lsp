#include "analysis/index/CodeActionFacts.h"

#include "analysis/index/CodeActionFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Computes the source range to remove a single declarator from a multi-declarator declaration.
 * @param[in] declarators List of all declarators in the declaration.
 * @param[in] declarator Target declarator node to remove.
 * @return Source range covering the declarator and its adjacent comma delimiter.
 */
static SourceRange ComputeMultiDeclaratorRemovalRange(const std::vector<TSNode>& declarators, TSNode declarator)
{
    size_t targetIdx = 0;
    for (size_t k = 0; k < declarators.size(); ++k)
    {
        if (ts_node_start_byte(declarators[k]) == ts_node_start_byte(declarator))
        {
            targetIdx = k;
            break;
        }
    }

    if (targetIdx == 0 && declarators.size() > 1)
    {
        const TSPoint startPt = ts_node_start_point(declarators[0]);
        const TSPoint endPt = ts_node_start_point(declarators[1]);
        return SourceRange{startPt.row, startPt.column, endPt.row, endPt.column};
    }
    else
    {
        const TSPoint startPt = ts_node_end_point(declarators[targetIdx - 1]);
        const TSPoint endPt = ts_node_end_point(declarators[targetIdx]);
        return SourceRange{startPt.row, startPt.column, endPt.row, endPt.column};
    }
}

std::optional<IndexedNodeSpan> FindNodeSpanAtPoint(const NodeIndex& index, uint32_t line, uint32_t column)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }
    const TSPoint p{line, column};
    const TSNode n = ts_node_descendant_for_point_range(rootNode, p, p);
    if (ts_node_is_null(n))
    {
        return std::nullopt;
    }
    const TSPoint start = ts_node_start_point(n);
    const TSPoint end = ts_node_end_point(n);
    return IndexedNodeSpan{
        IndexedByteRange{ts_node_start_byte(n), ts_node_end_byte(n)},
        SourceRange{start.row, start.column, end.row, end.column},
    };
}

IndexedDeclaratorRemoval FindUnusedDeclaratorRemoval(const NodeIndex& index, uint32_t line, uint32_t column)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return {};
    }

    const TSPoint pt{line, column};
    const TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);

    const auto& gs = parser::GetGrammarSymbols();
    const TSNode declarator = parser::FindAncestorWithSymbol(leaf, gs.symVariableDeclarator);
    const TSNode decl = parser::FindAncestorWithSymbol(declarator, gs.symVariableDeclaration);

    if (ts_node_is_null(decl))
    {
        return {};
    }

    std::vector<TSNode> declarators;
    TSTreeCursor cursor = ts_tree_cursor_new(decl);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            const TSNode child = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_null(child) && ts_node_symbol(child) == gs.symVariableDeclarator)
            {
                declarators.push_back(child);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);

    IndexedDeclaratorRemoval result;
    result.inDeclaration = true;
    result.declaratorCount = declarators.size();
    if (result.declaratorCount > 1)
    {
        result.multiRemovalRange = ComputeMultiDeclaratorRemovalRange(declarators, declarator);
    }
    return result;
}

TSPoint FindClassClosingBracePoint(TSNode classBody)
{
    if (ts_node_is_null(classBody))
    {
        return TSPoint{0, 0};
    }
    TSPoint closingPt = ts_node_end_point(classBody);
    TSTreeCursor cursor = ts_tree_cursor_new(classBody);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            if (ts_node_end_byte(ch) == ts_node_end_byte(classBody))
            {
                closingPt = ts_node_start_point(ch);
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return closingPt;
}

TSNode FindClassBodyNode(TSNode classNode)
{
    TSNode bodyNode = parser::GetChildByField(classNode, parser::GetGrammarSymbols().fieldBody);
    if (ts_node_is_null(bodyNode))
    {
        TSTreeCursor declCursor = ts_tree_cursor_new(classNode);
        if (ts_tree_cursor_goto_first_child(&declCursor))
        {
            do
            {
                TSNode ch = ts_tree_cursor_current_node(&declCursor);
                if (ts_node_symbol(ch) == parser::GetGrammarSymbols().symClassBody)
                {
                    bodyNode = ch;
                    break;
                }
            } while (ts_tree_cursor_goto_next_sibling(&declCursor));
        }
        ts_tree_cursor_delete(&declCursor);
    }
    return bodyNode;
}

std::optional<IndexedSourcePoint> FindClassOpenBracePoint(const NodeIndex& index, uint32_t classLine,
                                                          uint32_t classColumn)
{
    const TSNode root = index.Root();
    if (ts_node_is_null(root))
    {
        return std::nullopt;
    }
    const TSPoint cPt{classLine, classColumn};
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode cNode =
        parser::FindAncestorWithSymbol(ts_node_descendant_for_point_range(root, cPt, cPt), gs.symClassDeclaration);
    if (ts_node_is_null(cNode))
    {
        return std::nullopt;
    }

    const TSNode bodyNode = FindClassBodyNode(cNode);
    if (ts_node_is_null(bodyNode))
    {
        return std::nullopt;
    }

    std::optional<IndexedSourcePoint> result;
    TSTreeCursor cursor = ts_tree_cursor_new(bodyNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode bChild = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(bChild) == gs.tokOpenBrace)
            {
                const TSPoint pt = ts_node_end_point(bChild);
                result = IndexedSourcePoint{pt.row, pt.column};
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return result;
}

} // namespace angel_lsp::analysis
