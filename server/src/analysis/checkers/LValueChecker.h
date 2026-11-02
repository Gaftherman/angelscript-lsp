#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
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
    const Scope* scopeRoot = nullptr;

    /** @brief Optional single-pass AST node index. */
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Verifies that left-hand side targets of assignment expressions are modifiable l-values.
 * @param request Syntax tree, source code, and lexical scope.
 * @param ctx Diagnostic sink and context.
 */
void CheckLValues(const LValueCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Bundles callee candidate signatures, argument expressions, and typing state during call evaluation.
 */
struct CallCandidateContext
{
    const LValueCheckRequest& request;
    const Scope* scope = nullptr;
    DiagnosticContext& ctx;
};

/**
 * @brief Evaluation engine for validating assignable L-value expression nodes.
 */
struct LValueNodeValidator
{
    /**
     * @brief Determines whether an expression AST node denotes an assignable modifiable L-value.
     * @param[in] rawNode AST expression node to validate.
     * @param[in] scope Lexical scope.
     * @param[in] ctx Diagnostic context.
     * @return True if node is an assignable L-value; false otherwise.
     */
    static bool IsAssignableLValueNode(TSNode rawNode, const Scope* scope, const DiagnosticContext& ctx);
};
} // namespace angel_lsp::analysis
