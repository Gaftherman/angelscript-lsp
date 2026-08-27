#pragma once

#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include <tree_sitter/api.h>
#include <string>

namespace angel_lsp::analysis
{
    /**
     * @brief Request context for strict shared isolation boundary checking.
     */
    struct IsolationCheckRequest
    {
        TSNode rootNode;
        std::string_view sourceCode;
        const Scope *scopeRoot = nullptr;
    };

    /**
     * @brief Validates strict isolation boundaries: shared entities cannot access non-shared entities.
     * @param request Immutable context with AST, source code, and scopes.
     * @param ctx Diagnostic context to emit findings.
     */
    void CheckSharedIsolation(const IsolationCheckRequest &request, DiagnosticContext &ctx);
}
