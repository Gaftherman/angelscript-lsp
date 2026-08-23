#pragma once

#include "analysis/DiagnosticContext.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Checks what a function body does with control flow.
     *
     * The only pass here that reads statements rather than declarations. It needs the tree because
     * none of what it judges reaches the symbol table: whether a `break` sits inside a loop, whether
     * two `case` labels name the same value, whether every path out of a non-void function returns.
     *
     * These questions are answerable from one document alone - unlike almost everything else in
     * this analyzer, no host-registered type can change the answer - so the pass says what it finds
     * rather than falling silent on unresolved names.
     */
    struct ControlFlowCheckRequest
    {
        /** @brief Root node of the document's syntax tree. */
        TSNode root;

        /** @brief Document source text the tree was parsed from. */
        std::string_view sourceCode;
    };

    /**
     * @brief Walks the document's statements and reports control-flow errors.
     * @param request Tree and source text of the document under analysis.
     * @param ctx Diagnostic sink.
     */
    void CheckControlFlow(const ControlFlowCheckRequest &request, DiagnosticContext &ctx);
}
