#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/Utils.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves enclosing container symbols for an AST expression node.
 * @param[in] exprNode Target AST expression node.
 * @param[in] sourceCode Source text buffer.
 * @param[in] nodeIndex Optional precomputed node index.
 * @return Vector of enclosing container info records.
 */
inline std::vector<ContainerInfo> ResolveEnclosingContainers(TSNode exprNode, std::string_view sourceCode,
                                                             const NodeIndex* nodeIndex)
{
    if (nodeIndex && !ts_node_is_null(exprNode))
    {
        return GetEnclosingContainers(
            IndexedByteRange{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode)}, *nodeIndex, sourceCode);
    }
    return {};
}

/**
 * @brief Resolves symbols visible in lexical scope at the position of an AST node.
 * @param[in] name Symbol identifier name to look up.
 * @param[in] exprNode Target AST node providing position.
 * @param[in] sourceCode Source text buffer.
 * @param[in] symbolTable Global workspace symbol table.
 * @param[in] nodeIndex Optional precomputed node index.
 * @return Vector of matching symbols in scope.
 */
inline std::vector<Symbol> ResolveScopeSymbols(std::string_view name, TSNode exprNode, std::string_view sourceCode,
                                               const SymbolTable& symbolTable, const NodeIndex* nodeIndex)
{
    if (name.empty())
    {
        return {};
    }
    const uint32_t byteOffset = !ts_node_is_null(exprNode) ? ts_node_start_byte(exprNode) : 0;
    return FindSymbolsInScope(name, byteOffset, sourceCode, symbolTable, nodeIndex);
}

/**
 * @brief Returns trimmed text representation of an AST node.
 * @param[in] node AST node to read.
 * @param[in] sourceCode Source text buffer.
 * @return Trimmed string content of the node.
 */
inline std::string GetTrimmedNodeText(TSNode node, std::string_view sourceCode)
{
    return std::string(utils::TrimView(parser::NodeText(node, sourceCode)));
}

/**
 * @brief Evaluates the semantic type of an AST expression node.
 * @param[in] exprNode AST expression node to evaluate.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved type name or empty string.
 */
std::string ResolveASTExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx);

/**
 * @brief Evaluates primary or atomic AST expression nodes (literals, identifiers, this, parentheses).
 * @param[in] sym Tree-sitter grammar symbol.
 * @param[in] exprNode AST expression node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved type name if sym is primary, or std::nullopt.
 */
std::optional<std::string> ResolveASTPrimaryExpr(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx);

/**
 * @brief Evaluates composite AST expression nodes (binary, ternary, member, call, composite dispatcher).
 * @param[in] sym Tree-sitter grammar symbol.
 * @param[in] exprNode AST expression node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved type name or empty string.
 */
std::string ResolveASTCompositeExpr(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx);

/**
 * @brief Evaluates call AST expression nodes (member call, bare call, funcdef invocation).
 * @param[in] exprNode Call expression AST node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved return type name or empty string.
 */
std::string ResolveASTCallExpr(TSNode exprNode, const ExpressionTypeContext& ctx);

/**
 * @brief Evaluates secondary AST expression nodes (casts, constructors, index, lambda, assignments).
 * @param[in] sym Tree-sitter grammar symbol.
 * @param[in] exprNode AST expression node.
 * @param[in] ctx Semantic expression typing context.
 * @return Resolved type name or empty string.
 */
std::string ResolveASTSecondaryExpr(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
