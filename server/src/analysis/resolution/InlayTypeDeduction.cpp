#include "analysis/resolution/InlayTypeDeduction.h"

#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/AstRecoveryInternal.h"
#include "analysis/resolution/InlayTypeDeductionInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/Utils.h"

#include <cctype>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Trims surrounding whitespace, parentheses, and semicolons from node text view.
 * @param[in] text Raw text view to trim.
 * @return Trimmed text view.
 */
static std::string_view TrimNodeText(std::string_view text)
{
    std::string_view sv = utils::TrimView(text);
    while (!sv.empty() && (sv.front() == '(' || std::isspace(static_cast<unsigned char>(sv.front()))))
    {
        sv.remove_prefix(1);
        sv = utils::TrimView(sv);
    }
    while (!sv.empty() && (sv.back() == ')' || sv.back() == ';' || std::isspace(static_cast<unsigned char>(sv.back()))))
    {
        sv.remove_suffix(1);
        sv = utils::TrimView(sv);
    }
    return sv;
}

/**
 * @brief Deduces AngelScript type string from a numeric literal text.
 * @param[in] nodeTxt Trimmed literal string.
 * @return Deduced type ("float", "double", or "int").
 */
static std::string DeduceNumericLiteralType(std::string_view nodeTxt)
{
    if (nodeTxt.find('.') != std::string_view::npos || nodeTxt.find('e') != std::string_view::npos ||
        nodeTxt.find('E') != std::string_view::npos)
    {
        if (nodeTxt.ends_with('f') || nodeTxt.ends_with('F'))
        {
            return "float";
        }
        return "double";
    }
    if (nodeTxt.ends_with('f') || nodeTxt.ends_with('F'))
    {
        return "float";
    }
    if (nodeTxt.ends_with('d') || nodeTxt.ends_with('D'))
    {
        return "double";
    }
    return "int";
}

/**
 * @brief Deduces literal expression type from syntax node and source code.
 * @param[in] exprNode Literal AST node.
 * @param[in] sourceCode Source text buffer.
 * @param[in] stringTypeName Document string type name.
 * @return Type string or empty string.
 */
static std::string DeduceLiteralType(TSNode exprNode, std::string_view sourceCode, std::string_view stringTypeName)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(exprNode);
    const std::string_view nodeTxt = TrimNodeText(std::string(parser::NodeText(exprNode, sourceCode)));
    if (sym == gs.symNumberLiteral)
    {
        return DeduceNumericLiteralType(nodeTxt);
    }
    if (sym == gs.symStringLiteral || sym == gs.symConcatenatedString)
    {
        return std::string(stringTypeName);
    }
    if (sym == gs.symBooleanLiteral || nodeTxt == "true" || nodeTxt == "false")
    {
        return "bool";
    }
    return "";
}

std::string DeduceCastOrConstructType(TSNode exprNode, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol type = ts_node_symbol(exprNode);
    if (type == gs.symFunctionalCastExpression || type == gs.symCastExpression)
    {
        TSNode typeNode = parser::GetChildByField(exprNode, gs.fieldType);
        if (!ts_node_is_null(typeNode))
        {
            return std::string(parser::NodeText(typeNode, sourceCode));
        }
    }
    else if (type == gs.symConstructCallExpression)
    {
        TSNode typeNode = parser::GetChildByField(exprNode, gs.fieldType);
        if (!ts_node_is_null(typeNode))
        {
            std::string cType(parser::NodeText(typeNode, sourceCode));
            parser::ForEachChildNodeUntil(exprNode,
                                          [&](TSNode child)
                                          {
                                              if (ts_node_symbol(child) == gs.symTemplateTypeList)
                                              {
                                                  cType += std::string(parser::NodeText(child, sourceCode));
                                                  return false;
                                              }
                                              return true;
                                          });
            return cType;
        }
    }
    return "";
}

/**
 * @brief Deduces the type of an identifier or scoped identifier from scope or symbol table.
 * @param[in] exprNode Identifier AST node.
 * @param[in] ctx Semantic expression type context.
 * @param[in] scope Enclosing lexical scope, or nullptr.
 * @return Type string or empty string.
 */
static std::string DeduceIdentifierType(TSNode exprNode, const ExpressionTypeContext& ctx, const Scope* scope)
{
    std::string name(parser::NodeText(exprNode, ctx.sourceCode));
    if (scope)
    {
        const LocalDefinition* def = ResolveInScope(scope, name);
        if (def && !def->typeName.empty())
        {
            return def->typeName;
        }
    }
    auto symbols = ctx.symbolTable.FindSymbols(name);
    for (const auto& sym : symbols)
    {
        if (sym.type == SymbolType::Variable)
        {
            return sym.GetVariable().typeName;
        }
    }
    return "";
}

std::string DeduceExpressionTypeInternal(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (ts_node_is_null(exprNode))
    {
        return "";
    }

    const Scope* rootScope = ctx.scope;
    while (rootScope && rootScope->parent)
    {
        rootScope = rootScope->parent;
    }
    TSPoint point = ts_node_start_point(exprNode);
    const Scope* scope = rootScope ? FindInnermostScope(rootScope, point.row, point.column) : nullptr;

    const std::string_view strType = !ctx.stringTypeName.empty() ? ctx.stringTypeName : ctx.symbolTable.GetStringType();
    const std::string_view arrType =
        !ctx.arrayTypeName.empty() ? ctx.arrayTypeName : ctx.symbolTable.GetDefaultArrayType();

    ExpressionTypeContext exprCtx{scope, ctx.symbolTable, ctx.sourceCode, ctx.uri};
    exprCtx.nodeIndex = ctx.nodeIndex;
    exprCtx.stringTypeName = strType;
    exprCtx.arrayTypeName = arrType;
    std::string resolved = ResolveExpressionType(exprNode, exprCtx);
    if (!resolved.empty() && resolved != parser::keywords::Auto)
    {
        return resolved;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(exprNode);
    if (sym == gs.symParenthesizedExpression)
    {
        return DeduceParenthesizedType(exprNode, ctx);
    }
    std::string lit = DeduceLiteralType(exprNode, ctx.sourceCode, strType);
    if (!lit.empty())
    {
        return lit;
    }
    std::string cast = DeduceCastOrConstructType(exprNode, ctx.sourceCode);
    if (!cast.empty())
    {
        return cast;
    }
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        return DeduceIdentifierType(exprNode, ctx, scope);
    }
    return DeduceCompoundExpressionType(sym, exprNode, ctx, rootScope);
}

std::string DeduceExpressionTypeAt(uint32_t startByte, uint32_t endByte, const ExpressionTypeContext& ctx)
{
    TSNode exprNode = RecoverNodeAt(ctx, startByte, endByte);
    if (ts_node_is_null(exprNode))
    {
        return "";
    }
    return DeduceExpressionTypeInternal(exprNode, ctx);
}

} // namespace angel_lsp::analysis
