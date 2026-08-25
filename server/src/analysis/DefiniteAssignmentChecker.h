#pragma once

#include "analysis/DiagnosticContext.h"
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Context parameters for definite assignment dataflow analysis.
     */
    struct DefiniteAssignmentCheckRequest
    {
        TSNode root;
        std::string_view sourceCode;
        const Scope *scopeRoot = nullptr;
    };

    /**
     * @brief Analyzes dataflow in function bodies to detect reads of uninitialized local variables.
     * @param request Document AST root, source text, and scope tree.
     * @param ctx Diagnostic sink for emitting warnings/errors.
     */
    void CheckDefiniteAssignment(const DefiniteAssignmentCheckRequest &request, DiagnosticContext &ctx);
}
