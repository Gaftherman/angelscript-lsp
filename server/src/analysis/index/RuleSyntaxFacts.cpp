#include "analysis/index/RuleSyntaxFacts.h"

#include "analysis/index/EngineDialectFacts.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Extracts AST-free facts for rule validation from a precomputed node index.
 * @param[in] index Precomputed AST node index of the document.
 * @return Extracted rule syntax facts containing standalone lambdas and metadata spans.
 */
RuleSyntaxFacts BuildRuleSyntaxFacts(const NodeIndex& index)
{
    RuleSyntaxFacts facts;
    const auto& gs = parser::GetGrammarSymbols();

    for (const TSNode exprNode : index.Nodes(gs.symExpressionStatement))
    {
        if (ts_node_named_child_count(exprNode) == 1)
        {
            const TSNode child = ts_node_named_child(exprNode, 0);
            if (!ts_node_is_null(child) && ts_node_symbol(child) == gs.symLambdaExpression)
            {
                facts.standaloneLambdas.push_back(MakeIndexedSourceSpan(child));
            }
        }
    }

    const auto metaNodes = index.Nodes(gs.symMetadata);
    facts.metadata.reserve(metaNodes.size());
    for (const TSNode metaNode : metaNodes)
    {
        facts.metadata.push_back(MakeIndexedSourceSpan(metaNode));
    }

    return facts;
}

/**
 * @brief Checks whether a Tree-Sitter syntax tree contains any syntax errors.
 * @param[in] tree Pointer to the Tree-Sitter tree to inspect; may be null.
 * @return True when tree is non-null and its root node has a syntax error; false otherwise.
 */
bool TreeHasSyntaxError(const TSTree* tree) noexcept
{
    return tree != nullptr && ts_node_has_error(ts_tree_root_node(tree));
}

} // namespace angel_lsp::analysis
