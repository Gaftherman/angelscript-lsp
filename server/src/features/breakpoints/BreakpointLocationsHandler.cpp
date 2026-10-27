#include "BreakpointLocationsHandler.h"
#include "features/breakpoints/BreakpointStatementClassifier.h"

#include <algorithm>
#include <array>

namespace angel_lsp::features
{
namespace
{

constexpr uint32_t k_maxCursorDepth = 64;

void CollectFromNode(TSNode node, CollectorContext& ctx)
{
    if (IsNodeInvalid(node))
    {
        return;
    }
    const TSSymbol sym = ts_node_symbol(node);
    HandleFunctionLike(node, sym, ctx);

    if (HandleLoopsAndBranches(node, sym, ctx))
    {
        return;
    }
    HandleLeafStatement(node, sym, ctx);
}

void TraverseSubtree(TSTreeCursor& cursor, CollectorContext& ctx)
{
    uint32_t depth = 0;
    std::array<bool, k_maxCursorDepth + 1> blockAtDepth{};
    uint32_t blockCount = 0;

    while (true)
    {
        TSNode current = ts_tree_cursor_current_node(&cursor);
        TSPoint start = ts_node_start_point(current);
        TSPoint end = ts_node_end_point(current);
        const TSSymbol sym = ts_node_symbol(current);

        if (start.row <= ctx.req.endLine && end.row >= ctx.req.startLine)
        {
            ctx.inStatementBlock = (blockCount > 0);
            CollectFromNode(current, ctx);
            if (!ShouldSkipSubtree(sym, ctx.gs) && depth < k_maxCursorDepth && ts_tree_cursor_goto_first_child(&cursor))
            {
                const bool isBlock = (sym == ctx.gs.symStatementBlock);
                blockAtDepth[depth] = isBlock;
                if (isBlock)
                {
                    ++blockCount;
                }
                ++depth;
                continue;
            }
        }
        while (depth > 0 && !ts_tree_cursor_goto_next_sibling(&cursor))
        {
            ts_tree_cursor_goto_parent(&cursor);
            --depth;
            if (blockAtDepth[depth])
            {
                --blockCount;
                blockAtDepth[depth] = false;
            }
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
    CollectorContext ctx{request, gs, false, results};
    TSTreeCursor cursor = ts_tree_cursor_new(request.rootNode);
    TraverseSubtree(cursor, ctx);
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

    auto isDuplicate = [](const BreakpointLocationItem& a, const BreakpointLocationItem& b) noexcept
    { return a.line == b.line && a.column == b.column && a.endLine == b.endLine && a.endColumn == b.endColumn; };
    results.erase(std::unique(results.begin(), results.end(), isDuplicate), results.end());

    return results;
}

} // namespace angel_lsp::features
