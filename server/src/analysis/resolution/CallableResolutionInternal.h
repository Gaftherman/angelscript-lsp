#pragma once

#include "analysis/index/Symbol.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Extracts argument expression types from a call AST node.
 * @param[in] callNode AST call expression node.
 * @param[in] ctx Semantic expression context.
 * @return Vector of resolved argument type names.
 */
std::vector<std::string> ExtractCallArgumentTypes(TSNode callNode, const ExpressionTypeContext& ctx);

/**
 * @brief Collects constructor candidate symbols using a context AST node for scope resolution.
 * @param[in] baseName Base class name.
 * @param[in] contextNode Context AST node.
 * @param[in] sourceCode Document source text.
 * @param[in] symbolTable Symbol table.
 * @return Vector of constructor symbols.
 */
std::vector<Symbol> CollectConstructorCandidates(const std::string& baseName, TSNode contextNode,
                                                 std::string_view sourceCode, const SymbolTable& symbolTable);

/**
 * @brief Resolves funcdef target from a lambda AST node.
 * @param[in] lambdaNode AST lambda expression node.
 * @param[in] table Symbol table.
 * @param[in] sourceCode Document source text.
 * @return Target funcdef symbol if found.
 */
std::optional<Symbol> FuncdefTargetOfLambda(TSNode lambdaNode, const SymbolTable& table, std::string_view sourceCode);

/**
 * @brief Infers type of a lambda parameter from its enclosing lambda AST node.
 * @param[in] nodeInLambda AST node inside lambda.
 * @param[in] paramName Parameter identifier name.
 * @param[in] symbolTable Symbol table.
 * @param[in] sourceCode Document source text.
 * @return Inferred parameter type name or empty.
 */
std::string InferLambdaParamType(TSNode nodeInLambda, std::string_view paramName, const SymbolTable& symbolTable,
                                 std::string_view sourceCode);

/**
 * @brief Tests whether an AST expression denotes a function or method without invoking it.
 * @param[in] node AST expression node to inspect.
 * @param[in] ctx Semantic expression context.
 * @return True if node denotes a function symbol.
 */
[[nodiscard]] bool IsFunctionReference(TSNode node, const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
