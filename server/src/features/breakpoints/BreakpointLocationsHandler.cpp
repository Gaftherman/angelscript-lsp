#include "BreakpointLocationsHandler.h"

#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <array>

namespace angel_lsp::features
{
namespace
{

constexpr uint32_t k_maxCursorDepth = 64;

struct CollectorContext
{
    const BreakpointLocationsRequest& req;
    const parser::GrammarSymbols& gs;
    bool inStatementBlock = false;
    std::vector<BreakpointLocationItem>& out;
};

bool ShouldSkipSubtree(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.symEnumDeclaration || sym == gs.symInterfaceDeclaration || sym == gs.symImportDeclaration ||
           sym == gs.symTypedefDeclaration || sym == gs.symFuncdefDeclaration || sym == gs.symParameterList ||
           sym == gs.symTemplateTypeList || sym == gs.symTemplateParameterList || sym == gs.symComment ||
           sym == gs.symMetadata || sym == gs.symPreprocDirective || sym == gs.symBaseClassList;
}

bool HasVariableInitialization(TSNode varDecl, const parser::GrammarSymbols& gs) noexcept
{
    const uint32_t count = ts_node_child_count(varDecl);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(varDecl, i);
        if (ts_node_symbol(child) == gs.symVariableDeclarator)
        {
            if (!ts_node_is_null(ts_node_child_by_field_id(child, gs.fieldValue)) ||
                !ts_node_is_null(ts_node_child_by_field_id(child, gs.fieldArguments)))
            {
                return true;
            }
        }
    }
    return false;
}

bool IsPrimitiveOrHandleType(TSNode typeNode, const parser::GrammarSymbols& gs) noexcept
{
    if (ts_node_is_null(typeNode))
    {
        return false;
    }
    const uint32_t count = ts_node_child_count(typeNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(typeNode, i);
        const TSSymbol sym = ts_node_symbol(child);
        if (sym == gs.symPrimitiveType || sym == gs.tokAt)
        {
            return true;
        }
        if (ts_node_child_count(child) > 0 && IsPrimitiveOrHandleType(child, gs))
        {
            return true;
        }
    }
    return false;
}

bool IsExecutableVariableDecl(TSNode node, bool inStatementBlock, const parser::GrammarSymbols& gs) noexcept
{
    if (HasVariableInitialization(node, gs))
    {
        return true;
    }
    if (!inStatementBlock)
    {
        return false;
    }
    TSNode typeNode = ts_node_child_by_field_id(node, gs.fieldVarType);
    return !IsPrimitiveOrHandleType(typeNode, gs);
}

void AppendLocation(std::vector<BreakpointLocationItem>& out, TSPoint start, TSPoint end)
{
    out.push_back(BreakpointLocationItem{start.row, start.column, end.row, end.column});
}

void ProcessCompoundHeader(TSNode node, TSSymbol sym, const parser::GrammarSymbols& gs,
                           std::vector<BreakpointLocationItem>& out)
{
    TSPoint start = ts_node_start_point(node);
    TSPoint end = start;
    if (sym == gs.symIfStatement || sym == gs.symWhileStatement || sym == gs.symSwitchStatement)
    {
        TSNode cond = ts_node_child_by_field_id(node, gs.fieldCondition);
        end = !ts_node_is_null(cond) ? ts_node_end_point(cond) : start;
    }
    else if (sym == gs.symForStatement || sym == gs.symForeachStatement)
    {
        TSNode body = ts_node_child_by_field_id(node, gs.fieldBody);
        end = !ts_node_is_null(body) ? ts_node_start_point(body) : start;
    }
    if (end.row < start.row)
    {
        end = start;
    }
    AppendLocation(out, start, end);
}

void ProcessClosingBrace(TSNode node, const BreakpointLocationsRequest& req, const parser::GrammarSymbols& gs,
                         std::vector<BreakpointLocationItem>& out)
{
    TSNode body = ts_node_child_by_field_id(node, gs.fieldBody);
    if (ts_node_is_null(body) || ts_node_symbol(body) != gs.symStatementBlock)
    {
        return;
    }
    TSPoint endPt = ts_node_end_point(body);
    if (endPt.row < req.startLine || endPt.row > req.endLine ||
        (req.excludedRanges != nullptr && angel_lsp::utils::IsLineExcluded(*req.excludedRanges, endPt.row)))
    {
        return;
    }
    uint32_t startCol = endPt.column > 0 ? endPt.column - 1 : 0;
    AppendLocation(out, TSPoint{endPt.row, startCol}, endPt);
}

void ProcessDoWhileStatement(TSNode node, const BreakpointLocationsRequest& req, const parser::GrammarSymbols& gs,
                             std::vector<BreakpointLocationItem>& out)
{
    TSPoint start = ts_node_start_point(node);
    TSNode body = ts_node_child_by_field_id(node, gs.fieldBody);
    TSPoint doEnd = !ts_node_is_null(body) ? ts_node_start_point(body) : start;
    if (start.row >= req.startLine && start.row <= req.endLine &&
        (req.excludedRanges == nullptr || !angel_lsp::utils::IsLineExcluded(*req.excludedRanges, start.row)))
    {
        AppendLocation(out, start, doEnd);
    }
    TSNode cond = ts_node_child_by_field_id(node, gs.fieldCondition);
    if (!ts_node_is_null(cond))
    {
        TSPoint condStart = ts_node_start_point(cond);
        TSPoint condEnd = ts_node_end_point(cond);
        if (condStart.row >= req.startLine && condStart.row <= req.endLine &&
            (req.excludedRanges == nullptr || !angel_lsp::utils::IsLineExcluded(*req.excludedRanges, condStart.row)))
        {
            AppendLocation(out, condStart, condEnd);
        }
    }
}

void ProcessForStatementColumns(TSNode node, const parser::GrammarSymbols& gs, const BreakpointLocationsRequest& req,
                                std::vector<BreakpointLocationItem>& out)
{
    TSNode initNode = ts_node_child_by_field_id(node, gs.fieldInit);
    if (!ts_node_is_null(initNode) && ts_node_is_named(initNode))
    {
        TSPoint pt = ts_node_start_point(initNode);
        if (pt.row >= req.startLine && pt.row <= req.endLine)
        {
            AppendLocation(out, pt, ts_node_end_point(initNode));
        }
    }
    TSNode condNode = ts_node_child_by_field_id(node, gs.fieldCondition);
    if (!ts_node_is_null(condNode) && ts_node_is_named(condNode))
    {
        TSPoint pt = ts_node_start_point(condNode);
        if (pt.row >= req.startLine && pt.row <= req.endLine)
        {
            AppendLocation(out, pt, ts_node_end_point(condNode));
        }
    }
    TSNode updateNode = ts_node_child_by_field_id(node, gs.fieldUpdate);
    if (!ts_node_is_null(updateNode))
    {
        TSPoint pt = ts_node_start_point(updateNode);
        if (pt.row >= req.startLine && pt.row <= req.endLine)
        {
            AppendLocation(out, pt, ts_node_end_point(updateNode));
        }
    }
}

bool IsNodeInvalid(TSNode node) noexcept
{
    return ts_node_is_null(node) || ts_node_is_missing(node) || ts_node_is_error(node) ||
           ts_node_start_byte(node) >= ts_node_end_byte(node);
}

void HandleFunctionLike(TSNode node, TSSymbol sym, CollectorContext& ctx)
{
    if (sym == ctx.gs.symFuncDeclaration || sym == ctx.gs.symAccessor || sym == ctx.gs.symLambdaExpression)
    {
        ProcessClosingBrace(node, ctx.req, ctx.gs, ctx.out);
    }
}

bool HandleLoopsAndBranches(TSNode node, TSSymbol sym, CollectorContext& ctx)
{
    if (sym == ctx.gs.symDoWhileStatement)
    {
        ProcessDoWhileStatement(node, ctx.req, ctx.gs, ctx.out);
        return true;
    }
    if (sym == ctx.gs.symIfStatement || sym == ctx.gs.symWhileStatement || sym == ctx.gs.symForeachStatement ||
        sym == ctx.gs.symSwitchStatement)
    {
        ProcessCompoundHeader(node, sym, ctx.gs, ctx.out);
        return true;
    }
    if (sym == ctx.gs.symForStatement)
    {
        ProcessCompoundHeader(node, sym, ctx.gs, ctx.out);
        ProcessForStatementColumns(node, ctx.gs, ctx.req, ctx.out);
        return true;
    }
    return false;
}

void HandleLeafStatement(TSNode node, TSSymbol sym, CollectorContext& ctx)
{
    const TSPoint start = ts_node_start_point(node);
    if (sym == ctx.gs.symVariableDeclaration)
    {
        if (IsExecutableVariableDecl(node, ctx.inStatementBlock, ctx.gs))
        {
            AppendLocation(ctx.out, start, ts_node_end_point(node));
        }
        return;
    }
    if (sym == ctx.gs.symExpressionStatement)
    {
        if (ts_node_named_child_count(node) > 0)
        {
            AppendLocation(ctx.out, start, ts_node_end_point(node));
        }
        return;
    }
    if (sym == ctx.gs.symReturnStatement || sym == ctx.gs.symBreakStatement || sym == ctx.gs.symContinueStatement)
    {
        AppendLocation(ctx.out, start, ts_node_end_point(node));
    }
}

void CollectFromNode(TSNode node, CollectorContext& ctx)
{
    if (IsNodeInvalid(node))
    {
        return;
    }
    const TSSymbol sym = ts_node_symbol(node);
    HandleFunctionLike(node, sym, ctx);

    const TSPoint start = ts_node_start_point(node);
    if (start.row < ctx.req.startLine || start.row > ctx.req.endLine ||
        (ctx.req.excludedRanges != nullptr && angel_lsp::utils::IsLineExcluded(*ctx.req.excludedRanges, start.row)))
    {
        return;
    }
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