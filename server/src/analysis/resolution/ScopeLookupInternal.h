#pragma once

#include "analysis/index/Symbol.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"

#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Retrieves enclosing container chain from an AST node.
 */
std::vector<ContainerInfo> GetEnclosingContainers(TSNode node, std::string_view sourceCode,
                                                  const NodeIndex* nodeIndex = nullptr);

/**
 * @brief Collects using namespace directives from an AST root node.
 */
std::vector<std::string> CollectUsingNamespaces(TSNode root, std::string_view sourceCode);

/**
 * @brief Collects visible using namespace directives at an AST node.
 */
std::vector<std::string> CollectVisibleUsingNamespaces(TSNode node, std::string_view sourceCode,
                                                       const NodeIndex* nodeIndex);

/**
 * @brief Finds symbols in scope using an AST node for enclosing context.
 */
std::vector<Symbol> FindSymbolsInScope(const std::string& name, TSNode node, std::string_view sourceCode,
                                       const SymbolTable& symbolTable, const NodeIndex* nodeIndex = nullptr);

/**
 * @brief Identifies whether an AST node is a bare datatype in a value expression.
 */
[[nodiscard]] std::optional<std::string> IsBareDataType(TSNode node, const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
