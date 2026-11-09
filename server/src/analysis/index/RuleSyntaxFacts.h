#pragma once

#include "analysis/index/EngineDialectFacts.h"
#include "analysis/index/NodeIndex.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief AST-free syntactic facts required by language and directive validation rules.
 */
struct RuleSyntaxFacts
{
    /**
     * @brief Source spans of standalone lambda expressions within expression statements.
     */
    std::vector<IndexedSourceSpan> standaloneLambdas;

    /**
     * @brief Source spans of all metadata directive nodes.
     */
    std::vector<IndexedSourceSpan> metadata;
};

/**
 * @brief Extracts AST-free facts for rule validation from a precomputed node index.
 * @param[in] index Precomputed AST node index of the document.
 * @return Extracted rule syntax facts containing standalone lambdas and metadata spans.
 */
[[nodiscard]] RuleSyntaxFacts BuildRuleSyntaxFacts(const NodeIndex& index);

/**
 * @brief Checks whether a Tree-Sitter syntax tree contains any syntax errors.
 * @param[in] tree Pointer to the Tree-Sitter tree to inspect; may be null.
 * @return True when tree is non-null and its root node has a syntax error; false otherwise.
 */
[[nodiscard]] bool TreeHasSyntaxError(const TSTree* tree) noexcept;

} // namespace angel_lsp::analysis
