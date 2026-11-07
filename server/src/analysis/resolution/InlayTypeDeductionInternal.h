#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Deduces type from functional cast, C-style cast, or constructor call expressions.
 * @param[in] exprNode AST cast or constructor call node.
 * @param[in] sourceCode Source text buffer.
 * @return Type string or empty string.
 */
std::string DeduceCastOrConstructType(TSNode exprNode, std::string_view sourceCode);

/**
 * @brief Evaluates the expression type of an AST node using inlay deduction rules.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Semantic expression type context.
 * @return Deduced type string or empty string.
 */
std::string DeduceExpressionTypeInternal(TSNode exprNode, const ExpressionTypeContext& ctx);

/**
 * @brief Deduces compound expression type (call, member, unary, binary, postfix).
 * @param[in] type AST node grammar symbol.
 * @param[in] exprNode Target expression node.
 * @param[in] ctx Semantic expression type context.
 * @param[in] rootScope Root lexical scope.
 * @return Deduced type string or empty string.
 */
std::string DeduceCompoundExpressionType(TSSymbol type, TSNode exprNode, const ExpressionTypeContext& ctx,
                                         const Scope* rootScope);

/**
 * @brief Deduces type for a parenthesized expression AST node.
 * @param[in] exprNode Parenthesized expression node.
 * @param[in] ctx Semantic expression type context.
 * @return Deduced type string or empty string.
 */
std::string DeduceParenthesizedType(TSNode exprNode, const ExpressionTypeContext& ctx);

/**
 * @brief Deduces return type of a call expression.
 * @param[in] exprNode Call expression AST node.
 * @param[in] ctx Semantic expression type context.
 * @param[in] rootScope Root document scope.
 * @return Deduced return type string or empty string.
 */
std::string DeduceCallExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx, const Scope* rootScope);

/**
 * @brief Deduces the type of a member expression (property or method return type).
 * @param[in] exprNode Member expression AST node.
 * @param[in] ctx Semantic expression type context.
 * @param[in] rootScope Root document scope.
 * @return Deduced type string or empty string.
 */
std::string DeduceMemberExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx, const Scope* rootScope);

} // namespace angel_lsp::analysis
