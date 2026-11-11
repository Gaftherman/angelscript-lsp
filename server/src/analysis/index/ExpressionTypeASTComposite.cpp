#include "analysis/index/ExpressionTypeASTInternal.h"

#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/ExpressionTypeBinary.h"
#include "analysis/resolution/ExpressionTypeMember.h"
#include "analysis/resolution/ExpressionTypeTernary.h"
#include "analysis/resolution/ExpressionTypeUnary.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves binary expression type.
 * @param[in] exprNode Binary expression AST node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved expression type string or poison type.
 */
static std::string ResolveBinaryExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    std::vector<SyntaxOperands> chain;
    TSNode curr = exprNode;
    while (!ts_node_is_null(curr) && ts_node_symbol(curr) == gs.symBinaryExpression)
    {
        chain.emplace_back(curr, ctx.nodeIndex);
        curr = chain.back().Field(gs.fieldLeft);
    }

    if (chain.empty() || ts_node_is_null(curr))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }

    std::string currentType = ResolveASTExpressionType(curr, ctx);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        const TSNode opNode = it->Field(gs.fieldOperator);
        const TSNode right = it->Field(gs.fieldRight);
        if (ts_node_is_null(opNode) || ts_node_is_null(right))
        {
            return std::string(ExpressionTypeCache::k_poisonType);
        }

        std::string rightType = ResolveASTExpressionType(right, ctx);
        if (ExpressionTypeCache::IsPoisonType(currentType) || ExpressionTypeCache::IsPoisonType(rightType))
        {
            return std::string(ExpressionTypeCache::k_poisonType);
        }

        const std::string_view op = parser::OperatorSpelling(opNode);
        currentType = EvaluateBinaryResultType(op, currentType, rightType, ctx);
        if (currentType.empty())
        {
            break;
        }
    }
    return currentType;
}

/**
 * @brief Resolves ternary expression type.
 * @param[in] exprNode Ternary expression AST node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved branch type or poison sentinel.
 */
static std::string ResolveTernaryExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode consequence = syntax.Field(parser::GetGrammarSymbols().fieldConsequence);
    const TSNode alternative = syntax.Field(parser::GetGrammarSymbols().fieldAlternative);
    if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    std::string t1 = ResolveASTExpressionType(consequence, ctx);
    std::string t2 = ResolveASTExpressionType(alternative, ctx);
    if (ExpressionTypeCache::IsPoisonType(t1) || ExpressionTypeCache::IsPoisonType(t2))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    if (t1.empty())
    {
        return t2;
    }
    if (t2.empty())
    {
        return t1;
    }
    if (t1 == "void" || t2 == "void")
    {
        return "";
    }

    return ResolveTernaryBranchTypes(t1, t2, ctx.symbolTable);
}

/**
 * @brief Resolves member expression type.
 * @param[in] exprNode Member expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved member type, or empty string if unresolved.
 */
static std::string ResolveMemberExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode objNode = syntax.Field(parser::GetGrammarSymbols().fieldObject);
    const TSNode memNode = syntax.Field(parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }

    std::string objType = ResolveASTExpressionType(objNode, ctx);
    std::string fallbackObj = GetTrimmedNodeText(objNode, ctx.sourceCode);
    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);

    return ResolveMemberPropertyType(objType, fallbackObj, memName, ctx.symbolTable);
}

/**
 * @brief Resolves unary expression type.
 * @param[in] exprNode Unary expression AST node.
 * @param[in] ctx Semantic expression typing context.
 * @return Evaluated unary type name.
 */
static std::string ResolveUnaryExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode operandNode = syntax.Field(gs.fieldOperand);
    if (ts_node_is_null(operandNode) || ts_node_is_error(operandNode) || ts_node_is_missing(operandNode))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    const TSNode operatorNode = syntax.Field(gs.fieldOperator);
    const TSSymbol opSym = !ts_node_is_null(operatorNode) ? ts_node_symbol(operatorNode) : 0;
    if (opSym == gs.tokBang || opSym == gs.tokNot)
    {
        return "bool";
    }
    if (opSym == gs.tokAt)
    {
        std::string targetName = GetTrimmedNodeText(operandNode, ctx.sourceCode);
        std::vector<Symbol> inScope =
            ResolveScopeSymbols(targetName, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        bool isFuncTarget = IsFunctionHandleTargetName(targetName, inScope, ctx.symbolTable);
        std::string operandType = ResolveASTExpressionType(operandNode, ctx);
        return ResolveUnaryHandle(isFuncTarget, operandType);
    }
    std::string operandType = ResolveASTExpressionType(operandNode, ctx);
    if (ExpressionTypeCache::IsPoisonType(operandType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    if (opSym == gs.tokDash)
    {
        return ResolveUnaryMinus(operandType);
    }
    return operandType;
}

std::string ResolveASTCompositeExpr(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symBinaryExpression)
    {
        return ResolveBinaryExpr(exprNode, ctx);
    }
    if (sym == gs.symTernaryExpression)
    {
        return ResolveTernaryExpr(exprNode, ctx);
    }
    if (sym == gs.symMemberExpression)
    {
        return ResolveMemberExpr(exprNode, ctx);
    }
    if (sym == gs.symCallExpression)
    {
        return ResolveASTCallExpr(exprNode, ctx);
    }
    return ResolveASTSecondaryExpr(sym, exprNode, ctx);
}

} // namespace angel_lsp::analysis
