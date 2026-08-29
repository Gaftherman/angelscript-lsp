#pragma once

#include "analysis/DiagnosticContext.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Checks that every scope qualifier a document writes resolves to something.
     *
     * `Foo::bar` names a namespace, a class or an enum before the `::`. This pass walks every
     * scoped identifier and reports the ones whose prefix resolves to none of those - accounting
     * for `using namespace` directives, which make a qualifier optional rather than wrong.
     *
     * Extracted from SemanticAnalyzer, where it had grown into the largest of several rule passes
     * living inline in the orchestrator. Every other rule of this kind is a free function taking a
     * request struct; this one simply never got moved, and its size made SemanticAnalyzer look like
     * a checker rather than the thing that runs them.
     */
    struct NamespaceCheckRequest
    {
        /** @brief Root node of the document's syntax tree. */
        TSNode root;

        /** @brief Document source text the tree was parsed from. */
        std::string_view sourceCode;
    };

    /**
     * @brief Reports scope qualifiers and call targets that resolve to nothing.
     * @param request Tree and source text of the document under analysis.
     * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
     */
    void CheckNamespacesAndScopes(const NamespaceCheckRequest &request, DiagnosticContext &ctx);
}
