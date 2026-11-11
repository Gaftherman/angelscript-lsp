#include "analysis/index/ExpressionTypeASTRecovery.h"

#include "analysis/index/ExpressionTypeASTInternal.h"
#include "analysis/index/NodeRecoveryInternal.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"

#include <cctype>
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
 * @brief Reads trimmed text view of an AST node.
 * @param[in] node AST node to slice.
 * @param[in] sourceCode Source text buffer.
 * @return Trimmed string view into source buffer.
 */
static std::string_view GetTrimmedNodeTextView(TSNode node, std::string_view sourceCode) noexcept
{
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return {};
    }
    std::string_view sv = sourceCode.substr(start, end - start);
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.front())))
    {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.back())))
    {
        sv.remove_suffix(1);
    }
    return sv;
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
        return std::string(ExpressionTypeCache::k_poisonType);
    }

    const TSSymbol sym = ts_node_symbol(exprNode);
    if (ctx.cache)
    {
        const ExpressionCacheKey key{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode), sym};
        auto it = ctx.cache->types.find(key);
        if (it != ctx.cache->types.end())
        {
            return it->second;
        }

        std::string_view rawText = GetTrimmedNodeTextView(exprNode, ctx.sourceCode);
        if (rawText == "void")
        {
            return "void";
        }

        std::string resolved = ResolveExpressionTypeUncached(sym, exprNode, ctx);
        ctx.cache->types.emplace(key, resolved);
        return resolved;
    }

    std::string_view rawText = GetTrimmedNodeTextView(exprNode, ctx.sourceCode);
    if (rawText == "void")
    {
        return "void";
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
