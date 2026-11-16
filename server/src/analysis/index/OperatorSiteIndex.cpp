#include "analysis/index/OperatorSiteIndexInternal.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"

#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
static TSPoint FindOpenBracketPoint(TSNode indexNode, const parser::GrammarSymbols& gs) noexcept
{
    TSPoint fallback = ts_node_start_point(indexNode);
    TSTreeCursor cursor = ts_tree_cursor_new(indexNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            const TSNode child = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_named(child) && ts_node_symbol(child) == gs.tokOpenBracket)
            {
                fallback = ts_node_start_point(child);
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return fallback;
}

static bool IsLogicalBinaryToken(TSSymbol opSym, const parser::GrammarSymbols& gs) noexcept
{
    return opSym == gs.tokAmpAmp || opSym == gs.tokAnd || opSym == gs.tokPipePipe || opSym == gs.tokOr ||
           opSym == gs.tokCaretCaret || opSym == gs.tokXor;
}

static void CollectBinarySites(OperatorSiteBuilder& builder)
{
    const auto& gs = builder.gs;
    for (const TSNode node : builder.index.Nodes(gs.symBinaryExpression))
    {
        const TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
        const TSNode leftNode = parser::GetChildByField(node, gs.fieldLeft);
        const TSNode rightNode = parser::GetChildByField(node, gs.fieldRight);
        if (ts_node_is_null(opNode) || ts_node_is_null(leftNode) || ts_node_is_null(rightNode))
        {
            continue;
        }
        const TSSymbol opSym = ts_node_symbol(opNode);
        if (opSym == gs.tokIs || opSym == gs.tokBangIs)
        {
            continue;
        }
        if (IsLogicalBinaryToken(opSym, gs))
        {
            AppendBoolConditionSite(leftNode, builder);
            AppendBoolConditionSite(rightNode, builder);
            continue;
        }
        const TSPoint opPt = ts_node_start_point(opNode);
        const TSPoint scopePt = ts_node_start_point(node);
        IndexedOperatorSite site;
        site.kind = OperatorSiteKind::Binary;
        site.line = opPt.row;
        site.character = opPt.column;
        site.scopeLine = scopePt.row;
        site.scopeCharacter = scopePt.column;
        site.exprId = InternOperatorExpr(leftNode, builder);
        site.rhsExprId = InternOperatorExpr(rightNode, builder);
        site.op = std::string(parser::OperatorSpelling(opNode));
        site.leftIsHandle = SyntaxOperands(leftNode, &builder.index).HasUnaryOperator(gs.tokAt);
        site.rightIsHandle = SyntaxOperands(rightNode, &builder.index).HasUnaryOperator(gs.tokAt);
        builder.out.sites.push_back(std::move(site));
    }
}

static void CollectAssignmentSites(OperatorSiteBuilder& builder)
{
    const auto& gs = builder.gs;
    for (const TSNode node : builder.index.Nodes(gs.symAssignmentExpression))
    {
        const TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
        const TSNode leftNode = parser::GetChildByField(node, gs.fieldLeft);
        const TSNode rightNode = parser::GetChildByField(node, gs.fieldRight);
        if (ts_node_is_null(opNode) || ts_node_is_null(leftNode) || ts_node_is_null(rightNode))
        {
            continue;
        }
        const TSPoint opPt = ts_node_start_point(opNode);
        const TSPoint scopePt = ts_node_start_point(node);
        IndexedOperatorSite site;
        site.kind = OperatorSiteKind::Assignment;
        site.line = opPt.row;
        site.character = opPt.column;
        site.scopeLine = scopePt.row;
        site.scopeCharacter = scopePt.column;
        site.exprId = InternOperatorExpr(leftNode, builder);
        site.rhsExprId = InternOperatorExpr(rightNode, builder);
        site.op = std::string(parser::OperatorSpelling(opNode));
        site.leftIsHandle = SyntaxOperands(leftNode, &builder.index).HasUnaryOperator(gs.tokAt);
        builder.out.sites.push_back(std::move(site));
    }
}

static void CollectUnaryAndPostfixSites(OperatorSiteBuilder& builder)
{
    const auto& gs = builder.gs;
    auto visit = [&](TSNode node, bool isPostfix)
    {
        const TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
        const TSNode operandNode = parser::GetChildByField(node, gs.fieldOperand);
        if (ts_node_is_null(opNode) || ts_node_is_null(operandNode))
        {
            return;
        }
        const TSSymbol opSym = ts_node_symbol(opNode);
        if (!isPostfix && (opSym == gs.tokBang || opSym == gs.tokNot))
        {
            AppendBoolConditionSite(operandNode, builder);
            return;
        }
        if (opSym != gs.tokDash && opSym != gs.tokTilde && opSym != gs.tokPlusPlus && opSym != gs.tokDashDash)
        {
            return;
        }
        const TSPoint opPt = ts_node_start_point(opNode);
        const TSPoint scopePt = ts_node_start_point(node);
        IndexedOperatorSite site;
        site.kind = OperatorSiteKind::Unary;
        site.line = opPt.row;
        site.character = opPt.column;
        site.scopeLine = scopePt.row;
        site.scopeCharacter = scopePt.column;
        site.exprId = InternOperatorExpr(operandNode, builder);
        site.op = std::string(parser::OperatorSpelling(opNode));
        site.isPostfix = isPostfix;
        builder.out.sites.push_back(std::move(site));
    };

    for (const TSNode node : builder.index.Nodes(gs.symUnaryExpression))
    {
        visit(node, false);
    }
    for (const TSNode node : builder.index.Nodes(gs.symPostfixExpression))
    {
        visit(node, true);
    }
}

static void CollectIndexAndCastSites(OperatorSiteBuilder& builder)
{
    const auto& gs = builder.gs;
    for (const TSNode node : builder.index.Nodes(gs.symIndexExpression))
    {
        const uint32_t exprId = InternOperatorExpr(node, builder);
        if (exprId == UINT32_MAX)
        {
            continue;
        }
        const auto& expr = builder.out.exprs[exprId];
        const TSPoint bracketPt = FindOpenBracketPoint(node, gs);
        const TSPoint scopePt = ts_node_start_point(node);
        IndexedOperatorSite site;
        site.kind = OperatorSiteKind::Index;
        site.line = bracketPt.row;
        site.character = bracketPt.column;
        site.scopeLine = scopePt.row;
        site.scopeCharacter = scopePt.column;
        site.exprId = expr.left;
        site.argBegin = expr.argBegin;
        site.argCount = expr.argCount;
        builder.out.sites.push_back(std::move(site));
    }

    auto visitCast = [&](TSNode node, std::string_view kindOp)
    {
        const uint32_t exprId = InternOperatorExpr(node, builder);
        if (exprId == UINT32_MAX)
        {
            return;
        }
        const auto& expr = builder.out.exprs[exprId];
        const TSPoint pt = ts_node_start_point(node);
        IndexedOperatorSite site;
        site.kind = OperatorSiteKind::Cast;
        site.line = pt.row;
        site.character = pt.column;
        site.scopeLine = pt.row;
        site.scopeCharacter = pt.column;
        site.exprId = expr.left;
        site.op = std::string(kindOp);
        site.targetType = expr.text;
        builder.out.sites.push_back(std::move(site));
    };
    for (const TSNode node : builder.index.Nodes(gs.symCastExpression))
    {
        visitCast(node, "cast");
    }
    for (const TSNode node : builder.index.Nodes(gs.symFunctionalCastExpression))
    {
        visitCast(node, "conv");
    }
}

std::shared_ptr<const IndexedOperatorSites> BuildOperatorSites(const NodeIndex& index, std::string_view sourceCode)
{
    if (index.Empty() || sourceCode.empty())
    {
        return nullptr;
    }
    auto result = std::make_shared<IndexedOperatorSites>();
    OperatorSiteBuilder builder{index, sourceCode, parser::GetGrammarSymbols(), *result, {}};
    CollectBinarySites(builder);
    CollectAssignmentSites(builder);
    CollectUnaryAndPostfixSites(builder);
    CollectIndexAndCastSites(builder);
    CollectCallAndForeachSites(builder);
    CollectImplicitConversionAndUsingSites(builder);
    if (result->sites.empty() && result->usings.empty())
    {
        return nullptr;
    }
    return result;
}
} // namespace angel_lsp::analysis
