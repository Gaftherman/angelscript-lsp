#include "analysis/index/ExpressionTypeASTInternal.h"

#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/ExpressionTypeCast.h"
#include "analysis/resolution/ExpressionTypeIndex.h"
#include "analysis/resolution/ExpressionTypeUnary.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <optional>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves constructor call expression type.
 * @param[in] syntax Syntax operands of the construct call.
 * @param[in] sourceCode Source text buffer.
 * @return Resolved constructed type name.
 */
static std::string ResolveConstructCallExpr(const SyntaxOperands& syntax, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode typeNode = syntax.Field(gs.fieldType);
    if (ts_node_is_null(typeNode))
    {
        return "";
    }
    std::string_view written = parser::NodeText(typeNode, sourceCode);
    const TSNode templates = syntax.FirstNamed(gs.symTemplateTypeList);
    std::string_view tmplText = !ts_node_is_null(templates) ? parser::NodeText(templates, sourceCode) : "";
    return ResolveConstructCallType(written, tmplText);
}

/**
 * @brief Resolves casts and constructor calls from their written type operands.
 * @param[in] sym Tree-sitter grammar symbol.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved cast or construct type if applicable.
 */
static std::optional<std::string> ResolveCastOrConstructExpr(TSSymbol sym, TSNode exprNode,
                                                             const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symCastExpression || sym == gs.symFunctionalCastExpression)
    {
        const TSNode typeNode = SyntaxOperands(exprNode, ctx.nodeIndex).Field(gs.fieldType);
        return ts_node_is_null(typeNode) ? "" : ResolveCastExpressionType(parser::NodeText(typeNode, ctx.sourceCode));
    }
    if (sym == gs.symConstructCallExpression)
    {
        return ResolveConstructCallExpr(SyntaxOperands(exprNode, ctx.nodeIndex), ctx.sourceCode);
    }
    return std::nullopt;
}

/**
 * @brief Resolves initializer list or lambda expression types.
 * @param[in] sym Tree-sitter AST grammar symbol.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved expression type string if handled, std::nullopt otherwise.
 */
static std::optional<std::string> ResolveLambdaOrInitExpr(TSSymbol sym, TSNode exprNode,
                                                          const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symInitializerList)
    {
        return std::string(constants::k_initializerListTypeSentinel);
    }
    if (sym == gs.symLambdaExpression)
    {
        return ResolveLambdaTargetFuncdefType(ReadLambdaTarget(exprNode, ctx.sourceCode), ctx.symbolTable);
    }
    return std::nullopt;
}

/**
 * @brief Resolves indexed property accessor type from member expression receiver.
 * @param[in] objNode Member expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] indexArgType Inferred type of the index argument.
 * @return Resolved return type of the indexed getter, or empty string.
 */
static std::string ResolveMemberIndexedProperty(TSNode objNode, const ExpressionTypeContext& ctx,
                                                std::string_view indexArgType = "")
{
    const SyntaxOperands syntax(objNode, ctx.nodeIndex);
    const TSNode recNode = syntax.Field(parser::GetGrammarSymbols().fieldObject);
    const TSNode memNode = syntax.Field(parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(recNode) || ts_node_is_null(memNode))
    {
        return "";
    }
    std::string recType = CleanBaseType(ResolveASTExpressionType(recNode, ctx));
    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);
    return ResolveMemberIndexedPropertyType(recType, memName, ctx.symbolTable, indexArgType);
}

/**
 * @brief Resolves indexed property accessor type from scoped or bare identifier.
 * @param[in] objNode Scoped identifier or identifier AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] indexArgType Inferred type of the index argument.
 * @return Resolved return type of the indexed getter, or empty string.
 */
static std::string ResolveScopedOrBareIndexedProperty(TSNode objNode, const ExpressionTypeContext& ctx,
                                                      std::string_view indexArgType = "")
{
    const auto& gs = parser::GetGrammarSymbols();
    TSSymbol objSym = ts_node_symbol(objNode);
    if (objSym == gs.symScopedIdentifier)
    {
        std::string_view text = parser::NodeText(objNode, ctx.sourceCode);
        const auto [scopeName, name] = utils::SplitScopedName(text);
        return ResolveScopedIndexedPropertyType(scopeName, name, ctx.symbolTable, indexArgType);
    }

    std::string_view name = parser::NodeText(objNode, ctx.sourceCode);
    std::string enclosing;
    if (!ctx.uri.empty())
    {
        enclosing = ctx.symbolTable.FindEnclosingClassName(ctx.uri, ts_node_start_point(objNode).row);
    }
    return ResolveBareIndexedPropertyType(name, enclosing, ctx.symbolTable, indexArgType);
}

/**
 * @brief Resolves index expression types for containers and indexed property accessors.
 * @param[in] exprNode Index expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved indexed expression type, or empty string.
 */
static std::string ResolveIndexExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode objNode = syntax.Field(gs.fieldObject);
    if (ts_node_is_null(objNode))
    {
        return "";
    }

    const TSNode indexNode = syntax.Field(gs.fieldIndex);
    std::string indexArgType;
    if (!ts_node_is_null(indexNode))
    {
        indexArgType = ResolveASTExpressionType(indexNode, ctx);
    }

    if (ts_node_symbol(objNode) == gs.symMemberAccess)
    {
        if (std::string propType = ResolveMemberIndexedProperty(objNode, ctx, indexArgType); !propType.empty())
        {
            return propType;
        }
    }
    else
    {
        if (std::string propType = ResolveScopedOrBareIndexedProperty(objNode, ctx, indexArgType); !propType.empty())
        {
            return propType;
        }
    }

    std::string objType = ResolveASTExpressionType(objNode, ctx);
    if (objType.empty())
    {
        return "";
    }
    return ResolveIndexedTypeWithArg(objType, ctx.symbolTable, ctx.arrayTypeName, indexArgType);
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

std::string ResolveASTSecondaryExpr(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (auto castOrConstruct = ResolveCastOrConstructExpr(sym, exprNode, ctx))
    {
        return *castOrConstruct;
    }
    if (auto lambdaOrInit = ResolveLambdaOrInitExpr(sym, exprNode, ctx))
    {
        return *lambdaOrInit;
    }
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symAssignmentExpression)
    {
        TSNode left = SyntaxOperands(exprNode, ctx.nodeIndex).Field(gs.fieldLeft);
        return ts_node_is_null(left) ? "" : ResolveASTExpressionType(left, ctx);
    }
    if (sym == gs.symIndexExpression)
    {
        return ResolveIndexExpr(exprNode, ctx);
    }
    if (sym == gs.symUnaryExpression)
    {
        return ResolveUnaryExpr(exprNode, ctx);
    }
    if (sym == gs.symPostfixExpression)
    {
        TSNode operandNode = SyntaxOperands(exprNode, ctx.nodeIndex).Field(gs.fieldOperand);
        return ts_node_is_null(operandNode) ? "" : ResolveASTExpressionType(operandNode, ctx);
    }
    return "";
}

} // namespace angel_lsp::analysis
