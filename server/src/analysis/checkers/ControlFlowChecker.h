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

/**
 * @brief Analysis engine for determining definite return paths through AST control flow statements.
 */
struct DefiniteReturnAnalyzer
{
    /**
     * @brief Determines whether control definitely returns or terminates along all paths through the node.
     * @param[in] node AST statement or block node to inspect.
     * @param[in] sourceCode Source text of the document.
     * @return True if control definitely returns along all execution branches; false otherwise.
     */
    static bool DefinitelyReturns(TSNode node, std::string_view sourceCode);

    /**
     * @brief Evaluates whether a block or clause node definitely returns.
     * @param[in] node Block or clause AST node.
     * @param[in] sym Grammar symbol identifying the node.
     * @param[in] sourceCode Source text of the document.
     * @return True if block or clause definitely returns.
     */
    static bool BlockOrClauseDefinitelyReturns(TSNode node, TSSymbol sym, std::string_view sourceCode);

    /**
     * @brief Evaluates whether an if statement definitely returns on all branches.
     * @param[in] node If statement AST node.
     * @param[in] sourceCode Source text of the document.
     * @return True if both consequence and alternative definitely return.
     */
    static bool IfDefinitelyReturns(TSNode node, std::string_view sourceCode);

    /**
     * @brief Evaluates whether a switch statement definitely returns on all case branches.
     * @param[in] node Switch statement AST node.
     * @param[in] sourceCode Source text of the document.
     * @return True if switch statement definitely returns.
     */
    static bool SwitchDefinitelyReturns(TSNode node, std::string_view sourceCode);

    /**
     * @brief Evaluates whether a try/catch statement definitely returns.
     * @param[in] node Try statement AST node.
     * @param[in] sourceCode Source text of the document.
     * @return True if try block definitely returns.
     */
    static bool TryDefinitelyReturns(TSNode node, std::string_view sourceCode);
};
} // namespace angel_lsp::analysis
