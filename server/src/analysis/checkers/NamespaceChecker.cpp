#include "analysis/checkers/NamespaceChecker.h"

#include "analysis/checkers/NamespaceCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "parser/GrammarNames.h"

#include <array>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Dispatches node inspection to category-specific checks.
 * @param[in] node AST node to inspect.
 * @param[in] request Contextual request bundle containing AST root, source text, and node index.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
static void ProcessNamespaceNode(TSNode node, const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symScopedIdentifier)
    {
        CheckScopedIdentifier(node, request.sourceCode, ctx.request.symbolTable, ctx);
    }
    else if (sym == gs.symCallExpression)
    {
        CheckCallExpression(node, request, ctx);
    }
    else if (sym == gs.symImportDeclaration || ts_node_is_error(node))
    {
        CheckImportBody(node, request.sourceCode, ctx);
    }
}

/**
 * @brief Traverses pre-indexed nodes and runs namespace and scope checks.
 * @param[in] request Contextual request bundle containing AST root, source text, and node index.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
static void CheckFromNodeIndex(const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    request.nodeIndex->ForEachNodeOrdered(
        std::array{gs.symScopedIdentifier, gs.symCallExpression, gs.symImportDeclaration, gs.symError},
        [&](TSNode node) { ProcessNamespaceNode(node, request, ctx); });
}

/**
 * @brief Check from ast traversal.
 * @param[in] root Root AST node of the document or tree.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
static void CheckFromASTTraversal(TSNode root, const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(root))
    {
        return;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool visitedChildren = false;
    while (true)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (!visitedChildren)
        {
            ProcessNamespaceNode(node, request, ctx);
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                continue;
            }
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            visitedChildren = false;
            continue;
        }
        if (ts_tree_cursor_goto_parent(&cursor))
        {
            visitedChildren = true;
            continue;
        }
        break;
    }
    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Reports scope qualifiers and call targets that resolve to nothing.
 * @param[in] request Tree and source text of the document under analysis.
 * @param[in,out] ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void CheckNamespacesAndScopes(const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    if (request.nodeIndex)
    {
        CheckFromNodeIndex(request, ctx);
    }
    else
    {
        CheckFromASTTraversal(request.root, request, ctx);
    }
}

} // namespace angel_lsp::analysis
