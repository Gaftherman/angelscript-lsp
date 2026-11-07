#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/InlayTypeDeductionInternal.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <array>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if either operand matches target type name.
 */
static bool IsOperandMatch(std::string_view leftT, std::string_view rightT, std::string_view target) noexcept
{
    return leftT == target || rightT == target;
}

/**
 * @brief Selects the wider numeric/string type from binary operand types.
 * @param[in] leftT Left operand type string.
 * @param[in] rightT Right operand type string.
 * @param[in] stringTypeName Document string type name.
 * @return Wider type string.
 */
static std::string SelectWiderType(std::string_view leftT, std::string_view rightT, std::string_view stringTypeName)
{
    static constexpr std::array<std::string_view, 2> kFloatingTypes = {"double", "float"};
    for (const auto type : kFloatingTypes)
    {
        if (IsOperandMatch(leftT, rightT, type))
        {
            return std::string(type);
        }
    }

    if (!stringTypeName.empty() && IsOperandMatch(leftT, rightT, stringTypeName))
    {
        return std::string(stringTypeName);
    }

    static constexpr std::array<std::string_view, 2> kWideIntTypes = {"int64", "uint"};
    for (const auto type : kWideIntTypes)
    {
        if (IsOperandMatch(leftT, rightT, type))
        {
            return std::string(type);
        }
    }

    if (!leftT.empty())
    {
        return std::string(leftT);
    }
    return !rightT.empty() ? std::string(rightT) : "int";
}

/**
 * @brief Deduces result type of a binary expression AST node.
 * @param[in] exprNode Binary expression AST node.
 * @param[in] ctx Semantic expression type context.
 * @return Deduced type string.
 */
static std::string DeduceBinaryExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(exprNode, gs.fieldOperator);
    if (!ts_node_is_null(opNode) && IsBooleanBinaryOpSym(ts_node_symbol(opNode), gs))
    {
        return "bool";
    }

    TSNode left = parser::GetChildByField(exprNode, gs.fieldLeft);
    TSNode right = parser::GetChildByField(exprNode, gs.fieldRight);
    std::string leftT = DeduceExpressionTypeInternal(left, ctx);
    std::string rightT = DeduceExpressionTypeInternal(right, ctx);

    const std::string_view strType = !ctx.stringTypeName.empty() ? ctx.stringTypeName : ctx.symbolTable.GetStringType();
    return SelectWiderType(leftT, rightT, strType);
}

/**
 * @brief Deduces result type of a unary expression AST node.
 * @param[in] exprNode Unary expression AST node.
 * @param[in] ctx Semantic expression type context.
 * @return Deduced type string.
 */
static std::string DeduceUnaryExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(exprNode, gs.fieldOperator);
    const TSSymbol opSym = !ts_node_is_null(opNode) ? ts_node_symbol(opNode) : 0;
    TSNode operand = parser::GetChildByField(exprNode, gs.fieldOperand);

    if (opSym == gs.tokBang || opSym == gs.tokNot)
    {
        return "bool";
    }
    if (opSym == gs.tokAt)
    {
        std::string opT = DeduceExpressionTypeInternal(operand, ctx);
        if (!opT.empty() && !IsHandleTypeView(opT))
        {
            return opT + "@";
        }
        return opT;
    }
    return DeduceExpressionTypeInternal(operand, ctx);
}

std::string DeduceParenthesizedType(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    std::string result;
    const auto& gs = parser::GetGrammarSymbols();
    parser::ForEachChildNodeUntil(exprNode,
                                  [&](TSNode child)
                                  {
                                      const TSSymbol cType = ts_node_symbol(child);
                                      if (cType != gs.tokOpenParen && cType != gs.tokCloseParen)
                                      {
                                          result = DeduceExpressionTypeInternal(child, ctx);
                                          return false;
                                      }
                                      return true;
                                  });
    return result;
}

std::string DeduceCompoundExpressionType(TSSymbol type, TSNode exprNode, const ExpressionTypeContext& ctx,
                                         const Scope* rootScope)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (type == gs.symCallExpression)
    {
        return DeduceCallExpressionType(exprNode, ctx, rootScope);
    }
    if (type == gs.symMemberExpression)
    {
        return DeduceMemberExpressionType(exprNode, ctx, rootScope);
    }
    if (type == gs.symBinaryExpression)
    {
        return DeduceBinaryExpressionType(exprNode, ctx);
    }
    if (type == gs.symUnaryExpression)
    {
        return DeduceUnaryExpressionType(exprNode, ctx);
    }
    if (type == gs.symPostfixExpression)
    {
        TSNode operand = parser::GetChildByField(exprNode, gs.fieldOperand);
        return DeduceExpressionTypeInternal(operand, ctx);
    }
    return "";
}

} // namespace angel_lsp::analysis
