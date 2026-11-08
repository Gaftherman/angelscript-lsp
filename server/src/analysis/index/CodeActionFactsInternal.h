#pragma once

#include "analysis/index/ClassMutationFacts.h"
#include "analysis/index/NodeIndex.h"

#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Finds the source coordinate point of the closing brace of a class body.
 * @param[in] classBody Class body AST node to inspect.
 * @return Source point of the closing brace, or end point of class body.
 */
TSPoint FindClassClosingBracePoint(TSNode classBody);

/**
 * @brief Returns the body field of a class declaration, else its first class_body child, else a null node.
 * @param[in] classNode Class declaration AST node to inspect.
 * @return Class body AST node or a null node if not found.
 */
TSNode FindClassBodyNode(TSNode classNode);

/**
 * @brief Collects byte ranges of variable identifiers mutated within statements.
 * @param[in] stmts AST statement nodes to traverse.
 * @param[in] sourceCode Document source text buffer.
 * @param[out] mutatedNames Vector accumulating byte ranges of mutated identifier names.
 */
void CollectMutatedNameRanges(const std::vector<TSNode>& stmts, std::string_view sourceCode,
                              std::vector<IndexedByteRange>& mutatedNames);

/**
 * @brief Collects candidate class mutation sites and declared class fields for a method body.
 * @param[in] bodyNode Method body AST node to inspect.
 * @param[in] classNode Class declaration AST node.
 * @param[in] sourceCode Document source text buffer.
 * @param[out] facts Output facts structure populated with fields, sites, and body line bounds.
 */
void CollectMethodMutationFacts(TSNode bodyNode, TSNode classNode, std::string_view sourceCode,
                                IndexedMethodMutationFacts& facts);

} // namespace angel_lsp::analysis
