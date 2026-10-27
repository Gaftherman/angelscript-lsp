#include "features/breakpoints/BreakpointStatementClassifier.h"

#include <cstring>

namespace angel_lsp::features
{
namespace
{

bool IsConstantLiteralNode(TSNode node, const parser::GrammarSymbols& gs) noexcept
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

bool IsPureConstantDeclaration(TSNode node, TSNode typeNode, const parser::GrammarSymbols& gs) noexcept
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

void ProcessCompoundHeader(TSNode node, TSSymbol sym, CollectorContext& ctx)
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
    EmitLocationIfInRange(start, end, ctx.req, ctx.out);
}

void ProcessClosingBrace(TSNode node, CollectorContext& ctx)
{
    TSNode body = ts_node_child_by_field_id(node, ctx.gs.fieldBody);
    if (ts_node_is_null(body) || ts_node_symbol(body) != ctx.gs.symStatementBlock)
    {
        return;
    }
    TSPoint endPt = ts_node_end_point(body);
    uint32_t startCol = endPt.column > 0 ? endPt.column - 1 : 0;
    EmitLocationIfInRange(TSPoint{endPt.row, startCol}, endPt, ctx.req, ctx.out);
}

void ProcessDoWhileStatement(TSNode node, CollectorContext& ctx)
{
    TSPoint start = ts_node_start_point(node);
    TSNode body = ts_node_child_by_field_id(node, ctx.gs.fieldBody);
    TSPoint doEnd = !ts_node_is_null(body) ? ts_node_start_point(body) : start;
    EmitLocationIfInRange(start, doEnd, ctx.req, ctx.out);

    TSNode cond = ts_node_child_by_field_id(node, ctx.gs.fieldCondition);
    if (!ts_node_is_null(cond))
    {
        EmitLocationIfInRange(ts_node_start_point(cond), ts_node_end_point(cond), ctx.req, ctx.out);
    }
}

void ProcessForStatementColumns(TSNode node, CollectorContext& ctx)
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
            EmitLocationIfInRange(ts_node_start_point(child), ts_node_end_point(child), ctx.req, ctx.out);
        }
    }
}

} // namespace

bool ShouldSkipSubtree(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.symEnumDeclaration || sym == gs.symInterfaceDeclaration || sym == gs.symImportDeclaration ||
           sym == gs.symTypedefDeclaration || sym == gs.symFuncdefDeclaration || sym == gs.symParameterList ||
           sym == gs.symTemplateTypeList || sym == gs.symTemplateParameterList || sym == gs.symComment ||
           sym == gs.symMetadata || sym == gs.symPreprocDirective || sym == gs.symBaseClassList;
}

bool IsNodeInvalid(TSNode node) noexcept
{
    return ts_node_is_null(node) || ts_node_is_missing(node) || ts_node_is_error(node) ||
           ts_node_start_byte(node) >= ts_node_end_byte(node);
}

bool IsRowInRangeAndNotExcluded(uint32_t row, const BreakpointLocationsRequest& req) noexcept
{
    if (row < req.startLine || row > req.endLine)
    {
        return false;
    }
    if (req.excludedRanges != nullptr && angel_lsp::utils::IsLineExcluded(*req.excludedRanges, row))
    {
        return false;
    }
    return true;
}

void EmitLocationIfInRange(TSPoint start, TSPoint end, const BreakpointLocationsRequest& req,
                           std::vector<BreakpointLocationItem>& out)
{
    if (IsRowInRangeAndNotExcluded(start.row, req))
    {
        out.push_back(BreakpointLocationItem{start.row, start.column, end.row, end.column});
    }
}

bool IsBarePrimitiveOrHandleType(TSNode typeNode, const parser::GrammarSymbols& gs) noexcept
{
    if (ts_node_is_null(typeNode))
    {
        return false;
    }
    bool isHandle = false;
    bool isObjectArrayOrTemplate = false;
    bool isPrimitive = false;

    const uint32_t count = ts_node_child_count(typeNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(typeNode, i);
        const TSSymbol sym = ts_node_symbol(child);
        if (sym == gs.tokAt)
        {
            isHandle = true;
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
    return isHandle || isPrimitive;
}

bool IsExecutableVariableDecl(TSNode node, bool inStatementBlock, const parser::GrammarSymbols& gs) noexcept
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

void HandleFunctionLike(TSNode node, TSSymbol sym, CollectorContext& ctx)
{
    if (sym == ctx.gs.symFuncDeclaration || sym == ctx.gs.symAccessor || sym == ctx.gs.symLambdaExpression)
    {
        ProcessClosingBrace(node, ctx);
    }
}

bool HandleLoopsAndBranches(TSNode node, TSSymbol sym, CollectorContext& ctx)
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

void HandleLeafStatement(TSNode node, TSSymbol sym, CollectorContext& ctx)
{
    const TSPoint start = ts_node_start_point(node);
    if (sym == ctx.gs.symVariableDeclaration)
    {
        if (IsExecutableVariableDecl(node, ctx.inStatementBlock, ctx.gs))
        {
            EmitLocationIfInRange(start, ts_node_end_point(node), ctx.req, ctx.out);
        }
        return;
    }
    if (sym == ctx.gs.symExpressionStatement)
    {
        if (ts_node_named_child_count(node) > 0)
        {
            EmitLocationIfInRange(start, ts_node_end_point(node), ctx.req, ctx.out);
        }
        return;
    }
    if (sym == ctx.gs.symReturnStatement || sym == ctx.gs.symBreakStatement || sym == ctx.gs.symContinueStatement)
    {
        EmitLocationIfInRange(start, ts_node_end_point(node), ctx.req, ctx.out);
    }
}

} // namespace angel_lsp::features
