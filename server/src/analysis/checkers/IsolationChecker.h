#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
class NodeIndex;

/**
 * @brief Request context for strict shared isolation boundary checking.
 */
struct IsolationCheckRequest
{
    TSNode rootNode;
    std::string_view sourceCode;
    const Scope* scopeRoot = nullptr;
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Validates strict isolation boundaries: shared entities cannot access non-shared entities.
 * @param request Immutable context with AST, source code, and scopes.
 * @param ctx Diagnostic context to emit findings.
 */
void CheckSharedIsolation(const IsolationCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Expression node and evaluation barrier state during variable isolation analysis.
 */
struct IsoItem
{
    TSNode node;
    std::string className;
    bool isClassShared = false;
    bool inSharedContext = false;
};
} // namespace angel_lsp::analysis
