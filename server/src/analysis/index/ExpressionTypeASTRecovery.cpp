#include "analysis/index/ExpressionTypeASTRecovery.h"

#include "analysis/index/ExpressionTypeASTInternal.h"
#include "analysis/index/NodeRecoveryInternal.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "parser/Keywords.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Recovers an AST node from the analysis context given byte bounds.
 * @param[in] ctx Semantic expression typing context.
 * @param[in] startByte Inclusive start byte offset.
 * @param[in] endByte Exclusive end byte offset.
 * @return Recovered AST node, or null node if recovery fails.
 */
static TSNode RecoverNodeFromContext(const ExpressionTypeContext& ctx, uint32_t startByte, uint32_t endByte) noexcept
{
    if (ctx.nodeIndex)
    {
        return RecoverNodeAt(*ctx.nodeIndex, startByte, endByte);
    }
    return TSNode{};
}

/**
 * @brief Resolves expression type without cache lookup.
 * @param[in] sym Tree-sitter grammar symbol.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Semantic expression typing context.
 * @return Evaluated type name or empty string.
 */
static std::string ResolveExpressionTypeUncached(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (auto primary = ResolveASTPrimaryExpr(sym, exprNode, ctx))
    {
        return *primary;
    }
    return ResolveASTCompositeExpr(sym, exprNode, ctx);
}

std::string ResolveASTExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    exprNode = UnwrapIndexedParentheses(exprNode, ctx.nodeIndex);
    if (ts_node_is_null(exprNode) || ctx.sourceCode.empty())
    {
        return "";
    }
    if (ts_node_is_error(exprNode) || ts_node_is_missing(exprNode) ||
        (ctx.nodeIndex && ctx.nodeIndex->NodeHasSyntaxError(exprNode)))
    {
        return {};
    }

    const TSSymbol sym = ts_node_symbol(exprNode);
    if (sym == parser::GetGrammarSymbols().symVoidArgument)
    {
        return std::string(parser::keywords::Void);
    }
    if (ctx.cache)
    {
        const ExpressionCacheKey key{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode), sym};
        auto it = ctx.cache->types.find(key);
        if (it != ctx.cache->types.end())
        {
            return it->second;
        }

        std::string resolved = ResolveExpressionTypeUncached(sym, exprNode, ctx);
        ctx.cache->types.emplace(key, resolved);
        return resolved;
    }

    return ResolveExpressionTypeUncached(sym, exprNode, ctx);
}

std::string RecoverExpressionTypeFromAST(IndexedByteRange expressionRange, const ExpressionTypeContext& ctx)
{
    const TSNode node = RecoverNodeFromContext(ctx, expressionRange.startByte, expressionRange.endByte);
    if (ts_node_is_null(node))
    {
        return "";
    }
    return ResolveASTExpressionType(node, ctx);
}

} // namespace angel_lsp::analysis
