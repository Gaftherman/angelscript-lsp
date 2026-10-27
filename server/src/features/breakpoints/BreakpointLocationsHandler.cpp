#include "BreakpointLocationsHandler.h"

#include "parser/GrammarSymbols.h"

#include <algorithm>

namespace angel_lsp::features
{
namespace
{

constexpr uint32_t k_maxCursorDepth = 64;

bool IsExecutableStatement(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.symExpressionStatement || sym == gs.symReturnStatement || sym == gs.symBreakStatement ||
           sym == gs.symContinueStatement || sym == gs.symIfStatement || sym == gs.symForStatement ||
           sym == gs.symForeachStatement || sym == gs.symWhileStatement || sym == gs.symDoWhileStatement ||
           sym == gs.symSwitchStatement || sym == gs.symCaseClause || sym == gs.symTryStatement ||
           sym == gs.symVariableDeclaration;
}

void AppendLocation(std::vector<BreakpointLocationItem>& out, TSNode node)
{
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);
    out.push_back(BreakpointLocationItem{start.row, start.column, end.row, end.column});
}

void ProcessForStatementColumns(TSNode node, const parser::GrammarSymbols& gs, const BreakpointLocationsRequest& req,
                                std::vector<BreakpointLocationItem>& out)
{
    TSNode initNode = ts_node_child_by_field_id(node, gs.fieldInit);
    if (!ts_node_is_null(initNode))
    {
        TSPoint pt = ts_node_start_point(initNode);
        if (pt.row >= req.startLine && pt.row <= req.endLine)
        {
            AppendLocation(out, initNode);
        }
    }
    TSNode condNode = ts_node_child_by_field_id(node, gs.fieldCondition);
    if (!ts_node_is_null(condNode))
    {
        TSPoint pt = ts_node_start_point(condNode);
        if (pt.row >= req.startLine && pt.row <= req.endLine)
        {
            AppendLocation(out, condNode);
        }
    }
    TSNode updateNode = ts_node_child_by_field_id(node, gs.fieldUpdate);
    if (!ts_node_is_null(updateNode))
    {
        TSPoint pt = ts_node_start_point(updateNode);
        if (pt.row >= req.startLine && pt.row <= req.endLine)
        {
            AppendLocation(out, updateNode);
        }
    }
}

void CollectFromNode(TSNode node, const BreakpointLocationsRequest& req, const parser::GrammarSymbols& gs,
                     std::vector<BreakpointLocationItem>& out)
{
    TSSymbol sym = ts_node_symbol(node);
    if (!IsExecutableStatement(sym, gs))
    {
        return;
    }
    TSPoint start = ts_node_start_point(node);
    if (start.row < req.startLine || start.row > req.endLine ||
        (req.excludedRanges != nullptr && angel_lsp::utils::IsLineExcluded(*req.excludedRanges, start.row)))
    {
        return;
    }
    if (sym == gs.symForStatement)
    {
        ProcessForStatementColumns(node, gs, req, out);
        if (out.empty() || out.back().line != start.row)
        {
            AppendLocation(out, node);
        }
    }
    else
    {
        AppendLocation(out, node);
    }
}

void TraverseSubtree(TSTreeCursor& cursor, const BreakpointLocationsRequest& req, const parser::GrammarSymbols& gs,
                     std::vector<BreakpointLocationItem>& out)
{
    uint32_t depth = 0;
    while (true)
    {
        TSNode current = ts_tree_cursor_current_node(&cursor);
        TSPoint start = ts_node_start_point(current);
        TSPoint end = ts_node_end_point(current);

        if (start.row <= req.endLine && end.row >= req.startLine)
        {
            CollectFromNode(current, req, gs, out);
            if (depth < k_maxCursorDepth && ts_tree_cursor_goto_first_child(&cursor))
            {
                ++depth;
                continue;
            }
        }
        while (depth > 0 && !ts_tree_cursor_goto_next_sibling(&cursor))
        {
            ts_tree_cursor_goto_parent(&cursor);
            --depth;
        }
        if (depth == 0 && !ts_tree_cursor_goto_next_sibling(&cursor))
        {
            break;
        }
    }
}

} // namespace

std::vector<BreakpointLocationItem> BreakpointLocationsHandler::Collect(const BreakpointLocationsRequest& request)
{
    std::vector<BreakpointLocationItem> results;
    if (ts_node_is_null(request.rootNode) || request.startLine > request.endLine)
    {
        return results;
    }

    const auto& gs = parser::GetGrammarSymbols();
    TSTreeCursor cursor = ts_tree_cursor_new(request.rootNode);
    TraverseSubtree(cursor, request, gs, results);
    ts_tree_cursor_delete(&cursor);

    std::sort(results.begin(), results.end(),
              [](const BreakpointLocationItem& a, const BreakpointLocationItem& b) noexcept
              {
                  if (a.line != b.line)
                      return a.line < b.line;
                  if (a.column != b.column)
                      return a.column < b.column;
                  if (a.endLine != b.endLine)
                      return a.endLine < b.endLine;
                  return a.endColumn < b.endColumn;
              });

    results.erase(std::unique(results.begin(), results.end(),
                              [](const BreakpointLocationItem& a, const BreakpointLocationItem& b) noexcept
                              {
                                  return a.line == b.line && a.column == b.column && a.endLine == b.endLine &&
                                         a.endColumn == b.endColumn;
                              }),
                  results.end());

    return results;
}

} // namespace angel_lsp::features
