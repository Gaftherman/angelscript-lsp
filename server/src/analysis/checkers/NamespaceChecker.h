#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks that every scope qualifier a document writes resolves to something.
 *        `Foo::bar` names a namespace, a class or an enum before the `::`. This pass walks every
 */

struct NamespaceCheckRequest
{
    /**
     * @brief Root node of the document's syntax tree.
     */
    TSNode root;

    /**
     * @brief Document source text the tree was parsed from.
     */
    std::string_view sourceCode;

    /**
     * @brief Optional pre-indexed node index for fast single-pass querying.
     */
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Reports scope qualifiers and call targets that resolve to nothing.
 * @param request Tree and source text of the document under analysis.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void CheckNamespacesAndScopes(const NamespaceCheckRequest& request, DiagnosticContext& ctx);
} // namespace angel_lsp::analysis
