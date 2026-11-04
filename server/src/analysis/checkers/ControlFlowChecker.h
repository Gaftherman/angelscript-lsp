#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks what a function body does with control flow.
 *        The only pass here that reads statements rather than declarations. It needs the tree because
 */
struct ControlFlowCheckRequest
{
    /**
     * @brief Root node of the document's syntax tree.
     */
    TSNode root;

    /**
     * @brief Document source text the tree was parsed from.
     */
    std::string_view sourceCode;
};

/**
 * @brief Walks the document's statements and reports control-flow errors.
 * @param request Tree and source text of the document under analysis.
 * @param ctx Diagnostic sink.
 */
void CheckControlFlow(const ControlFlowCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Tracks branch termination, return paths, and unreachable code states across AST blocks.
 */
struct ControlFlowState
{
    uint32_t loopDepth = 0;
    uint32_t switchDepth = 0;

    /**
     * @brief What the enclosing function must return, and the name to say it under.
     *        Empty means there is nothing to check against - a constructor, or a lambda whose
     */
    std::string requiredReturn;
    std::string functionName;

    /**
     * @brief True for a function the grammar gives no return type: a constructor or a
     *        destructor.
     */
    bool implicitVoid = false;
};

} // namespace angel_lsp::analysis
