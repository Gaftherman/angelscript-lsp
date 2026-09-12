#pragma once

#include "analysis/DiagnosticContext.h"
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    class NodeIndex;

    /**
     * @brief Request context for l-value correctness checking.
     */
    struct LValueCheckRequest
    {
        /** @brief Root node of the document's syntax tree. */
        TSNode root;

        /** @brief Document source text the tree was parsed from. */
        std::string_view sourceCode;

        /** @brief Root of the document's lexical scope tree, or nullptr when none was collected. */
        const Scope *scopeRoot = nullptr;

        /** @brief Optional single-pass AST node index. */
        const NodeIndex *nodeIndex = nullptr;
    };

    /**
     * @brief Verifies that left-hand side targets of assignment expressions are modifiable l-values.
     * @param request Syntax tree, source code, and lexical scope.
     * @param ctx Diagnostic sink and context.
     */
    void CheckLValues(const LValueCheckRequest &request, DiagnosticContext &ctx);
}
