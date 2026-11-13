#include "analysis/index/ExpressionTypeASTInternal.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/ExpressionTypeIdentifiers.h"
#include "analysis/resolution/ExpressionTypeLiterals.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/ScopedNameUtils.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves literal expression type from grammar symbol.
 * @param[in] nodeSym Tree-sitter grammar symbol of literal.
 * @param[in] exprNode Literal AST node.
 * @param[in] sourceCode Source text buffer.
 * @param[in] stringTypeName Document string type name.
 * @return Resolved literal type name or empty string.
 */
static std::string ResolveLiteralExpr(TSSymbol nodeSym, TSNode exprNode, std::string_view sourceCode,
                                      std::string_view stringTypeName)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (nodeSym == gs.symStringLiteral)
    {
        return std::string(stringTypeName);
    }
    if (nodeSym == gs.symBooleanLiteral)
    {
        return "bool";
    }
    if (nodeSym == gs.symNullLiteral)
    {
        return "null";
    }
    if (nodeSym == gs.symNumberLiteral)
    {
        return std::string(ResolveNumberLiteral(parser::NodeText(exprNode, sourceCode)));
    }
    return "";
}

/**
 * @brief Resolves parenthesized expression type by unwrapping inner nodes.
 * @param[in] exprNode Parenthesized AST expression node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved inner type name or empty string.
 */
static std::string ResolveParenthesizedExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode inner = UnwrapIndexedParentheses(exprNode, ctx.nodeIndex);
    if (!ts_node_is_null(inner) && ts_node_symbol(inner) != gs.symParenthesizedExpression)
    {
        return ResolveASTExpressionType(inner, ctx);
    }
    return "";
}

/**
 * @brief Resolves 'this' expression type based on enclosing container hierarchy or mixin host.
 * @param[in] exprNode Target this AST node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved this type name or empty string.
 */
static std::string ResolveThisExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const bool isVirtual = ctx.uri.starts_with("angelscript-virtual:") || ctx.uri.starts_with("angelscript-virtual://");
    std::string hostClass = isVirtual ? SymbolTable::ExtractVirtualHostClass(ctx.uri) : "";
    if (hostClass.empty())
    {
        hostClass =
            InferHostClassFromEnclosingMixin(IndexedByteRange{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode)},
                                             ctx.nodeIndex, ctx.sourceCode, ctx.symbolTable);
    }
    auto containers = ResolveEnclosingContainers(exprNode, ctx.sourceCode, ctx.nodeIndex);
    return ResolveThisType(hostClass, containers);
}

/**
 * @brief Resolves scoped identifier expression type.
 * @param[in] exprNode Scoped identifier AST node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved identifier type name or empty string.
 */
static std::string ResolveScopedIdentifierExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto text = parser::NodeText(exprNode, ctx.sourceCode);
    const auto [qualView, mem] = utils::SplitScopedName(utils::StripGlobalPrefix(text));
    if (!qualView.empty())
    {
        auto wholeSyms = ResolveScopeSymbols(text, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        auto qualSyms = ResolveScopeSymbols(qualView, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        return ResolveScopedIdentifierType(text, wholeSyms, qualSyms, ctx.symbolTable);
    }

    TSNode lastIdentifier =
        SyntaxOperands(exprNode, ctx.nodeIndex).LastNamed(parser::GetGrammarSymbols().symIdentifier);
    return ts_node_is_null(lastIdentifier) ? std::string() : ResolveASTExpressionType(lastIdentifier, ctx);
}

/**
 * @brief Finds symbol matches for a bare identifier.
 * @param[in] name Identifier name.
 * @param[in] exprNode AST syntax node.
 * @param[in] ctx Expression type resolution context.
 * @return Candidate symbols in lexical, member, or global scope.
 */
static std::vector<Symbol> ResolveIdentifierSymbols(const std::string& name, TSNode exprNode,
                                                    const ExpressionTypeContext& ctx)
{
    auto syms = ResolveScopeSymbols(name, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
    if (!syms.empty())
    {
        return syms;
    }

    auto containers = ResolveEnclosingContainers(exprNode, ctx.sourceCode, ctx.nodeIndex);
    syms = ResolveEnclosingContainerMemberSymbols(name, containers, ctx.symbolTable, ctx.cache);
    if (!syms.empty())
    {
        return syms;
    }

    return ctx.symbolTable.FindSymbols(name);
}

/**
 * @brief Host class fallback resolution for identifiers in mixin classes and virtual documents.
 * @param[in] name Identifier name.
 * @param[in] exprNode AST syntax node.
 * @param[in] ctx Semantic expression typing context.
 * @return Inferred type or empty string.
 */
static std::string ResolveIdentifierHostFallback(const std::string& name, TSNode exprNode,
                                                 const ExpressionTypeContext& ctx)
{
    std::string hostClass;
    if (ctx.uri.starts_with("angelscript-virtual:") || ctx.uri.starts_with("angelscript-virtual://"))
    {
        hostClass = SymbolTable::ExtractVirtualHostClass(ctx.uri);
    }
    else
    {
        hostClass =
            InferHostClassFromEnclosingMixin(IndexedByteRange{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode)},
                                             ctx.nodeIndex, ctx.sourceCode, ctx.symbolTable);
    }
    return ResolveIdentifierHostFallbackFromHierarchy(name, hostClass, ctx.symbolTable, ctx.cache);
}

/**
 * @brief Resolves base class delegate property for an identifier.
 * @param[in] name Property name.
 * @param[in] exprNode AST syntax node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved property type or empty string.
 */
static std::string ResolveIdentifierDelegateProperty(std::string_view name, TSNode exprNode,
                                                     const ExpressionTypeContext& ctx)
{
    if (!IsBaseClassDelegateProperty(name, ctx.symbolTable))
    {
        return "";
    }
    const bool isVirtual = ctx.uri.starts_with("angelscript-virtual:") || ctx.uri.starts_with("angelscript-virtual://");
    std::string hostClass = isVirtual ? SymbolTable::ExtractVirtualHostClass(ctx.uri) : "";
    std::string baseClass =
        ResolveDelegateTargetBaseClass({IndexedByteRange{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode)},
                                        ctx.nodeIndex, ctx.sourceCode, ctx.symbolTable, hostClass, nullptr});
    if (!baseClass.empty())
    {
        return MemberOwnerType(baseClass, ctx.arrayTypeName);
    }
    return "";
}

/**
 * @brief Resolves bare identifier expression type.
 * @param[in] exprNode Identifier AST node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved identifier type name or empty string.
 */
static std::string ResolveIdentifierExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    std::string name(parser::NodeText(exprNode, ctx.sourceCode));
    if (name == "this")
    {
        return ResolveThisExpr(exprNode, ctx);
    }
    if (ctx.scope)
    {
        const LocalDefinition* def = ResolveInScopeAt(ctx.scope, name, ts_node_start_byte(exprNode));
        if (def)
        {
            std::string dType = ResolveLocalDefinitionType(*def, ctx);
            if (!dType.empty())
            {
                return CleanExpressionType(dType);
            }
        }
    }
    auto syms = ResolveIdentifierSymbols(name, exprNode, ctx);
    if (syms.empty())
    {
        if (auto hostRes = ResolveIdentifierHostFallback(name, exprNode, ctx); !hostRes.empty())
        {
            return hostRes;
        }
    }
    if (std::string firstType = ExtractFirstSymbolTypeOrAccessor(syms, ctx.symbolTable); !firstType.empty())
    {
        return firstType;
    }
    if (auto accessorRes = ResolveIdentifierAccessorFallback(name, ctx.symbolTable); !accessorRes.empty())
    {
        return accessorRes;
    }
    return ResolveIdentifierDelegateProperty(name, exprNode, ctx);
}

std::optional<std::string> ResolveASTPrimaryExpr(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symParenthesizedExpression)
    {
        return ResolveParenthesizedExpr(exprNode, ctx);
    }
    if (sym == gs.symTypedInitializerList)
    {
        TSNode typeNode = SyntaxOperands(exprNode, ctx.nodeIndex).Field(gs.fieldType);
        if (!ts_node_is_null(typeNode))
        {
            return std::string(parser::NodeText(typeNode, ctx.sourceCode));
        }
    }
    if (sym == gs.symThisExpression)
    {
        return ResolveThisExpr(exprNode, ctx);
    }
    if (auto lit = ResolveLiteralExpr(sym, exprNode, ctx.sourceCode, ctx.stringTypeName); !lit.empty())
    {
        return lit;
    }
    if (sym == gs.symScopedIdentifier)
    {
        return ResolveScopedIdentifierExpr(exprNode, ctx);
    }
    if (sym == gs.symIdentifier || sym == gs.tokFunction || parser::keywords::IsKeyword(parser::GetNodeType(exprNode)))
    {
        return ResolveIdentifierExpr(exprNode, ctx);
    }
    return std::nullopt;
}

} // namespace angel_lsp::analysis
