#include "analysis/index/ExtractMethodFacts.h"

#include "analysis/index/CodeActionFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Finds the enclosing function node for a selection range.
 * @param[in] rootNode AST root node.
 * @param[in] range Selection range.
 * @return Function declaration node or null node.
 */
static TSNode FindEnclosingFunctionNode(TSNode rootNode, const SourceRange& range)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSPoint startPt{static_cast<uint32_t>(range.startLine), static_cast<uint32_t>(range.startCharacter)};
    TSPoint endPt{static_cast<uint32_t>(range.endLine), static_cast<uint32_t>(range.endCharacter)};
    TSNode selNode = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
    return parser::FindAncestorWithSymbol(selNode, gs.symFuncDeclaration);
}

/**
 * @brief Finds the statement block body of a function node.
 * @param[in] fnNode Function declaration node.
 * @return Statement block node or null node.
 */
static TSNode FindFunctionBodyBlock(TSNode fnNode)
{
    TSNode bodyNode = parser::GetChildByField(fnNode, parser::GetGrammarSymbols().fieldBody);
    if (!ts_node_is_null(bodyNode))
    {
        return bodyNode;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(fnNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(ch) == parser::GetGrammarSymbols().symStatementBlock)
            {
                bodyNode = ch;
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return bodyNode;
}

/**
 * @brief Collects statements within a block that intersect the selection range.
 * @param[in] bodyNode Statement block node.
 * @param[in] range Selection range.
 * @return Vector of intersecting statement nodes.
 */
static std::vector<TSNode> CollectStatementsInRange(TSNode bodyNode, const SourceRange& range)
{
    std::vector<TSNode> selectedStmts;
    TSTreeCursor cursor = ts_tree_cursor_new(bodyNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_named(ch))
            {
                continue;
            }
            const TSSymbol t = ts_node_symbol(ch);
            const auto& gs = parser::GetGrammarSymbols();
            if (t == gs.tokOpenBrace || t == gs.tokCloseBrace)
            {
                continue;
            }
            TSPoint cStart = ts_node_start_point(ch);
            TSPoint cEnd = ts_node_end_point(ch);
            if (cStart.row <= range.endLine && cEnd.row >= range.startLine)
            {
                selectedStmts.push_back(ch);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return selectedStmts;
}

/**
 * @brief Finds the enclosing class declaration node for a function node.
 * @param[in] fnNode Function declaration node.
 * @return Class declaration node or null node.
 */
static TSNode FindEnclosingClassNode(TSNode fnNode)
{
    const auto& gs = parser::GetGrammarSymbols();
    return parser::FindAncestorWithSymbol(fnNode, gs.symClassDeclaration);
}

static TSNode FindInnermostStatementBlock(TSNode rootNode, const SourceRange& range, TSNode fnNode)
{
    TSPoint startPt{static_cast<uint32_t>(range.startLine), static_cast<uint32_t>(range.startCharacter)};
    TSPoint endPt{static_cast<uint32_t>(range.endLine), static_cast<uint32_t>(range.endCharacter)};
    TSNode node = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
    const auto& gs = parser::GetGrammarSymbols();
    TSNode result{};
    parser::ForEachAncestor(node,
                            [&](TSNode cur) -> bool
                            {
                                if (ts_node_eq(cur, fnNode))
                                {
                                    return false;
                                }
                                if (ts_node_symbol(cur) == gs.symStatementBlock)
                                {
                                    result = cur;
                                    return false;
                                }
                                return true;
                            });
    return !ts_node_is_null(result) ? result : FindFunctionBodyBlock(fnNode);
}

/**
 * @brief Finds the insertion point for adding a method definition inside a class.
 * @param[in] classNode Class declaration node.
 * @return Insertion point.
 */
static TSPoint FindClassBodyInsertionPoint(TSNode classNode)
{
    return FindClassClosingBracePoint(FindClassBodyNode(classNode));
}

std::optional<IndexedExtractMethodSelection>
FindExtractMethodSelection(const NodeIndex& index, std::string_view sourceCode, const SourceRange& selection)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    if (selection.startLine == selection.endLine && selection.startCharacter == selection.endCharacter)
    {
        return std::nullopt;
    }

    const TSNode fnNode = FindEnclosingFunctionNode(rootNode, selection);
    if (ts_node_is_null(fnNode))
    {
        return std::nullopt;
    }

    const TSNode bodyNode = FindInnermostStatementBlock(rootNode, selection, fnNode);
    if (ts_node_is_null(bodyNode))
    {
        return std::nullopt;
    }

    const std::vector<TSNode> selected = CollectStatementsInRange(bodyNode, selection);
    if (selected.empty())
    {
        return std::nullopt;
    }

    IndexedExtractMethodSelection site;
    site.functionStartLine = ts_node_start_point(fnNode).row;
    site.functionEndLine = ts_node_end_point(fnNode).row;

    const TSNode classNode = FindEnclosingClassNode(fnNode);
    site.inClass = !ts_node_is_null(classNode);
    if (site.inClass)
    {
        const TSPoint classInsertPt = FindClassBodyInsertionPoint(classNode);
        site.classInsertLine = classInsertPt.row;
        site.classInsertColumn = classInsertPt.column;
    }

    const TSNode firstStmt = selected.front();
    const TSNode lastStmt = selected.back();
    site.statements = IndexedByteRange{ts_node_start_byte(firstStmt), ts_node_end_byte(lastStmt)};

    const TSPoint frontPt = ts_node_start_point(firstStmt);
    const TSPoint backPt = ts_node_end_point(lastStmt);
    site.statementsRange = SourceRange{frontPt.row, frontPt.column, backPt.row, backPt.column};

    const auto& gs = parser::GetGrammarSymbols();
    site.endsWithReturn = ts_node_symbol(lastStmt) == gs.symReturnStatement;

    const TSNode retTypeNode = parser::GetChildByField(fnNode, gs.fieldReturnType);
    if (!ts_node_is_null(retTypeNode))
    {
        site.functionReturnType = IndexedByteRange{ts_node_start_byte(retTypeNode), ts_node_end_byte(retTypeNode)};
    }

    CollectMutatedNameRanges(selected, sourceCode, site.mutatedNames);

    return site;
}

} // namespace angel_lsp::analysis
