#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ControlFlowFacts.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks what a function body does with control flow.
 */
struct ControlFlowCheckRequest
{
    /**
     * @brief Document source text the tree was parsed from.
     */
    std::string_view sourceCode{};

    /**
     * @brief Precomputed node index of the document (optional).
     */
    const NodeIndex* nodeIndex = nullptr;

    /**
     * @brief Precomputed control flow facts (optional).
     */
    const ControlFlowFacts* facts = nullptr;
};

/**
 * @brief Walks the document's statements and reports control-flow errors from a request.
 * @param[in] request Tree, node index, facts, and source text of the document under analysis.
 * @param[in,out] ctx Diagnostic sink.
 */
void CheckControlFlow(const ControlFlowCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Validates control flow directly against precalculated pass-1 control flow facts.
 * @param[in] facts Precalculated pass-1 control flow facts.
 * @param[in] sourceCode Document source text.
 * @param[in,out] ctx Diagnostic sink.
 */
void CheckControlFlow(const ControlFlowFacts& facts, std::string_view sourceCode, DiagnosticContext& ctx);

/**
 * @brief Validates control flow directly against precalculated pass-1 control flow facts.
 * @param[in] facts Precalculated pass-1 control flow facts.
 * @param[in,out] ctx Diagnostic sink.
 */
void CheckControlFlow(const ControlFlowFacts& facts, DiagnosticContext& ctx);

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
     *        target funcdef could not be resolved.
     */
    std::string_view requiredReturn{};
    std::string_view functionName{};

    /**
     * @brief True for a function the grammar gives no return type: a constructor or a destructor.
     */
    bool implicitVoid = false;
};

} // namespace angel_lsp::analysis
