#include "analysis/index/BreakpointFacts.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
static bool IsConstantLiteralNode(TSNode node, const parser::GrammarSymbols& gs) noexcept
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symNumberLiteral || sym == gs.symBooleanLiteral || sym == gs.symStringLiteral)
    {
        return true;
    }
    if (sym == gs.symUnaryExpression)
    {
        TSNode operand = ts_node_child_by_field_id(node, gs.fieldOperand);
        if (!ts_node_is_null(operand))
        {
            const TSSymbol opSym = ts_node_symbol(operand);
            return (opSym == gs.symNumberLiteral || opSym == gs.symBooleanLiteral);
        }
    }
    return false;
}

static bool IsBarePrimitiveOrHandleType(TSNode typeNode, const parser::GrammarSymbols& gs) noexcept
{
    if (ts_node_is_null(typeNode))
    {
        return false;
    }
    bool hasHandleModifier = false;
    bool isObjectArrayOrTemplate = false;
    bool isPrimitive = false;

    const uint32_t count = ts_node_child_count(typeNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(typeNode, i);
        const TSSymbol sym = ts_node_symbol(child);
        if (sym == gs.tokAt)
        {
            hasHandleModifier = true;
        }
        else if (sym == gs.tokOpenBracket || sym == gs.symTemplateTypeList)
        {
            isObjectArrayOrTemplate = true;
        }
        else if (sym == gs.symDatatype)
        {
            const uint32_t dtCount = ts_node_child_count(child);
            for (uint32_t j = 0; j < dtCount; ++j)
            {
                if (ts_node_symbol(ts_node_child(child, j)) == gs.symPrimitiveType)
                {
                    isPrimitive = true;
                    break;
                }
            }
        }
        else if (sym == gs.symPrimitiveType)
        {
            isPrimitive = true;
        }
    }

    if (isObjectArrayOrTemplate)
    {
        return false;
    }
    return hasHandleModifier || isPrimitive;
}

static bool IsPureConstantDeclaration(TSNode node, TSNode typeNode, const parser::GrammarSymbols& gs) noexcept
{
    if (ts_node_is_null(typeNode) || !IsBarePrimitiveOrHandleType(typeNode, gs))
    {
        return false;
    }
    bool hasConst = false;
    const uint32_t typeChildCount = ts_node_child_count(typeNode);
    for (uint32_t i = 0; i < typeChildCount; ++i)
    {
        if (ts_node_symbol(ts_node_child(typeNode, i)) == gs.tokConst)
        {
            hasConst = true;
            break;
        }
    }
    if (!hasConst)
    {
        return false;
    }
    uint32_t declaratorCount = 0;
    const uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (ts_node_symbol(child) == gs.symVariableDeclarator)
        {
            ++declaratorCount;
            TSNode val = ts_node_child_by_field_id(child, gs.fieldValue);
            if (!IsConstantLiteralNode(val, gs))
            {
                return false;
            }
        }
    }
    return declaratorCount > 0;
}

static bool HasVariableInitialization(TSNode varDecl, const parser::GrammarSymbols& gs) noexcept
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

static bool IsExecutableVariableDecl(TSNode node, bool inStatementBlock, const parser::GrammarSymbols& gs) noexcept
{
    TSNode typeNode = ts_node_child_by_field_id(node, gs.fieldVarType);
    if (IsPureConstantDeclaration(node, typeNode, gs))
    {
        return false;
    }
    if (HasVariableInitialization(node, gs))
    {
        return true;
    }
    if (!inStatementBlock)
    {
        return false;
    }
    return !IsBarePrimitiveOrHandleType(typeNode, gs);
}

static bool IsNodeInvalid(TSNode node) noexcept
{
    return ts_node_is_null(node) || ts_node_is_missing(node) || ts_node_is_error(node) ||
           ts_node_start_byte(node) >= ts_node_end_byte(node);
}

static void EmitSpanIfInRange(TSPoint start, TSPoint end, BreakpointSpanContext& ctx)
{
    if (start.row >= ctx.startLine && start.row <= ctx.endLine)
    {
        ctx.out.push_back(SourceRange{start.row, start.column, end.row, end.column});
    }
}

static void ProcessCompoundHeader(TSNode node, TSSymbol sym, BreakpointSpanContext& ctx)
{
    TSPoint start = ts_node_start_point(node);
    TSPoint end = start;
    if (sym == ctx.gs.symIfStatement || sym == ctx.gs.symWhileStatement || sym == ctx.gs.symSwitchStatement)
    {
        TSNode cond = ts_node_child_by_field_id(node, ctx.gs.fieldCondition);
        end = !ts_node_is_null(cond) ? ts_node_end_point(cond) : start;
    }
    else if (sym == ctx.gs.symForStatement || sym == ctx.gs.symForeachStatement)
    {
        TSNode body = ts_node_child_by_field_id(node, ctx.gs.fieldBody);
        end = !ts_node_is_null(body) ? ts_node_start_point(body) : start;
    }
    if (end.row < start.row)
    {
        end = start;
    }
    EmitSpanIfInRange(start, end, ctx);
}

static void ProcessClosingBrace(TSNode node, BreakpointSpanContext& ctx)
{
    TSNode body = ts_node_child_by_field_id(node, ctx.gs.fieldBody);
    if (ts_node_is_null(body) || ts_node_symbol(body) != ctx.gs.symStatementBlock)
    {
        return;
    }
    TSPoint endPt = ts_node_end_point(body);
    uint32_t startCol = endPt.column > 0 ? endPt.column - 1 : 0;
    EmitSpanIfInRange(TSPoint{endPt.row, startCol}, endPt, ctx);
}

static void ProcessDoWhileStatement(TSNode node, BreakpointSpanContext& ctx)
{
    TSPoint start = ts_node_start_point(node);
    TSNode body = ts_node_child_by_field_id(node, ctx.gs.fieldBody);
    TSPoint doEnd = !ts_node_is_null(body) ? ts_node_start_point(body) : start;
    EmitSpanIfInRange(start, doEnd, ctx);

    TSNode cond = ts_node_child_by_field_id(node, ctx.gs.fieldCondition);
    if (!ts_node_is_null(cond))
    {
        EmitSpanIfInRange(ts_node_start_point(cond), ts_node_end_point(cond), ctx);
    }
}

static void ProcessForStatementColumns(TSNode node, BreakpointSpanContext& ctx)
{
    const uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (!ts_node_is_named(child))
        {
            continue;
        }
        const char* fieldName = ts_node_field_name_for_child(node, i);
        if (fieldName == nullptr)
        {
            continue;
        }
        if (std::strcmp(fieldName, "init") == 0 || std::strcmp(fieldName, "condition") == 0 ||
            std::strcmp(fieldName, "update") == 0)
        {
            EmitSpanIfInRange(ts_node_start_point(child), ts_node_end_point(child), ctx);
        }
    }
}

static void HandleFunctionLike(TSNode node, TSSymbol sym, BreakpointSpanContext& ctx)
{
    if (sym == ctx.gs.symFuncDeclaration || sym == ctx.gs.symAccessor || sym == ctx.gs.symLambdaExpression)
    {
        ProcessClosingBrace(node, ctx);
    }
}

static bool HandleLoopsAndBranches(TSNode node, TSSymbol sym, BreakpointSpanContext& ctx)
{
    if (sym == ctx.gs.symDoWhileStatement)
    {
        ProcessDoWhileStatement(node, ctx);
        return true;
    }
    if (sym == ctx.gs.symIfStatement || sym == ctx.gs.symWhileStatement || sym == ctx.gs.symForeachStatement ||
        sym == ctx.gs.symSwitchStatement)
    {
        ProcessCompoundHeader(node, sym, ctx);
        return true;
    }
    if (sym == ctx.gs.symForStatement)
    {
        ProcessCompoundHeader(node, sym, ctx);
        ProcessForStatementColumns(node, ctx);
        return true;
    }
    return false;
}

static void HandleLeafStatement(TSNode node, TSSymbol sym, BreakpointSpanContext& ctx)
{
    const TSPoint start = ts_node_start_point(node);
    if (sym == ctx.gs.symVariableDeclaration)
    {
        if (IsExecutableVariableDecl(node, ctx.inStatementBlock, ctx.gs))
        {
            EmitSpanIfInRange(start, ts_node_end_point(node), ctx);
        }
        return;
    }
    if (sym == ctx.gs.symExpressionStatement)
    {
        if (ts_node_named_child_count(node) > 0)
        {
            EmitSpanIfInRange(start, ts_node_end_point(node), ctx);
        }
        return;
    }
    if (sym == ctx.gs.symReturnStatement || sym == ctx.gs.symBreakStatement || sym == ctx.gs.symContinueStatement)
    {
        EmitSpanIfInRange(start, ts_node_end_point(node), ctx);
    }
}

std::vector<SourceRange> CollectBreakpointSpans(const NodeIndex& index, uint32_t startLine, uint32_t endLine)
{
    std::vector<SourceRange> results;
    if (startLine > endLine)
    {
        return results;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 14> targetSymbols = {
        gs.symFuncDeclaration,     gs.symAccessor,
        gs.symLambdaExpression,    gs.symDoWhileStatement,
        gs.symIfStatement,         gs.symWhileStatement,
        gs.symForeachStatement,    gs.symSwitchStatement,
        gs.symForStatement,        gs.symVariableDeclaration,
        gs.symExpressionStatement, gs.symReturnStatement,
        gs.symBreakStatement,      gs.symContinueStatement,
    };

    BreakpointSpanContext ctx{gs, startLine, endLine, results, false};

    index.ForEachNodeOrdered(targetSymbols,
                             [&](TSNode current)
                             {
                                 if (IsNodeInvalid(current))
                                 {
                                     return;
                                 }
                                 const TSPoint start = ts_node_start_point(current);
                                 const TSPoint end = ts_node_end_point(current);
                                 if (start.row > endLine && end.row > endLine)
                                 {
                                     return;
                                 }
                                 if (start.row < startLine && end.row < startLine)
                                 {
                                     return;
                                 }

                                 const TSSymbol sym = ts_node_symbol(current);
                                 HandleFunctionLike(current, sym, ctx);

                                 if (HandleLoopsAndBranches(current, sym, ctx))
                                 {
                                     return;
                                 }

                                 if (sym == gs.symVariableDeclaration)
                                 {
                                     ctx.inStatementBlock = !ts_node_is_null(
                                         parser::FindAncestorWithSymbol(current, gs.symStatementBlock));
                                 }
                                 HandleLeafStatement(current, sym, ctx);
                             });

    return results;
}

static bool IsNodeInsideCallFunction(TSNode memberExpr, const parser::GrammarSymbols& gs)
{
    TSNode parent = parser::GetParent(memberExpr);
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symCallExpression)
    {
        return false;
    }
    TSNode fnChild = ts_node_child_by_field_id(parent, gs.fieldFunction);
    return !ts_node_is_null(fnChild) && ts_node_eq(fnChild, memberExpr);
}

static IndexedEvaluatableSpan MakeEvaluatableSpan(TSNode node)
{
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);
    IndexedEvaluatableSpan span;
    span.range = SourceRange{start.row, start.column, end.row, end.column};
    span.startByte = ts_node_start_byte(node);
    span.endByte = ts_node_end_byte(node);
    return span;
}

static std::optional<IndexedEvaluatableSpan> HandleMemberExpression(TSNode leaf, TSNode parent,
                                                                    const parser::GrammarSymbols& gs)
{
    TSNode objNode = ts_node_child_by_field_id(parent, gs.fieldObject);
    if (!ts_node_is_null(objNode) && ts_node_eq(objNode, leaf))
    {
        return MakeEvaluatableSpan(leaf);
    }

    TSNode memberNode = ts_node_child_by_field_id(parent, gs.fieldMember);
    if (!ts_node_is_null(memberNode) && ts_node_eq(memberNode, leaf))
    {
        if (IsNodeInsideCallFunction(parent, gs))
        {
            return std::nullopt;
        }
        return MakeEvaluatableSpan(parent);
    }

    return MakeEvaluatableSpan(leaf);
}

static std::optional<IndexedEvaluatableSpan> HandleIndexExpression(TSNode leaf, TSNode parent,
                                                                   const parser::GrammarSymbols& gs)
{
    TSNode objNode = ts_node_child_by_field_id(parent, gs.fieldObject);
    if (!ts_node_is_null(objNode) && ts_node_eq(objNode, leaf))
    {
        return MakeEvaluatableSpan(leaf);
    }
    return MakeEvaluatableSpan(parent);
}

static bool IsIgnoredNode(TSNode node, const parser::GrammarSymbols& gs)
{
    if (ts_node_is_null(node))
    {
        return true;
    }
    const TSSymbol sym = ts_node_symbol(node);
    return sym == gs.symComment || sym == gs.symStringLiteral || sym == gs.tokOpenBrace || sym == gs.tokCloseBrace;
}

std::optional<IndexedEvaluatableSpan> FindEvaluatableSpan(const NodeIndex& index, uint32_t line, uint32_t column)
{
    TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    TSPoint point{line, column};
    TSNode leaf = ts_node_descendant_for_point_range(rootNode, point, point);
    if (ts_node_is_null(leaf))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    if (IsIgnoredNode(leaf, gs))
    {
        return std::nullopt;
    }

    TSNode parent = parser::GetParent(leaf);
    const TSSymbol leafSym = ts_node_symbol(leaf);

    if (leafSym == gs.tokOpenBracket || leafSym == gs.tokCloseBracket)
    {
        if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symIndexExpression)
        {
            return MakeEvaluatableSpan(parent);
        }
        return std::nullopt;
    }

    if (!ts_node_is_null(parent))
    {
        TSSymbol parentSym = ts_node_symbol(parent);
        if (parentSym == gs.symCallExpression)
        {
            TSNode fnNode = ts_node_child_by_field_id(parent, gs.fieldFunction);
            if (!ts_node_is_null(fnNode) && ts_node_eq(fnNode, leaf))
            {
                return std::nullopt;
            }
        }
        else if (parentSym == gs.symMemberExpression)
        {
            return HandleMemberExpression(leaf, parent, gs);
        }
        else if (parentSym == gs.symIndexExpression)
        {
            return HandleIndexExpression(leaf, parent, gs);
        }
    }

    return MakeEvaluatableSpan(leaf);
}

} // namespace angel_lsp::analysis
