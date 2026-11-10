#pragma once

#include "analysis/checkers/ControlFlowChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ControlFlowFacts.h"
#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Validates case labels within a switch statement for duplicate values and constantness.
 * @param[in] labels Pre-indexed case label facts.
 * @param[in] sourceCode Source text of the document under analysis.
 * @param[in,out] ctx Diagnostic collection context for emitting invalid or duplicate case errors.
 */
void CheckSwitchLabels(std::span<const ControlFlowCaseLabelFact> labels, std::string_view sourceCode,
                       DiagnosticContext& ctx);

/**
 * @brief Validates case labels within a switch statement record for duplicate values and constantness.
 * @param[in] record Switch statement record to evaluate.
 * @param[in] facts AST-free control flow facts.
 * @param[in] sourceCode Source text of the document under analysis.
 * @param[in,out] ctx Diagnostic collection context for emitting invalid or duplicate case errors.
 */
void CheckSwitchLabels(const ControlFlowRecord& record, const ControlFlowFacts& facts, std::string_view sourceCode,
                       DiagnosticContext& ctx);

/**
 * @brief Structure holding state and parameters for ControlFlowVisitor.
 */
class ControlFlowVisitor
{
  public:
    /**
     * @brief Constructs a visitor instance bound to the active source, diagnostic sink, and flow facts.
     * @param[in] sourceCode Document source text.
     * @param[in,out] ctx Diagnostic collection context.
     * @param[in] facts AST-free control flow facts.
     */
    ControlFlowVisitor(std::string_view sourceCode, DiagnosticContext& ctx, const ControlFlowFacts& facts);

    /**
     * @brief Runs control-flow validation across document statements in ordered sequence.
     */
    void Run();

    /**
     * @brief Validates return statements against enclosing return requirements.
     * @param[in] record AST-free statement record for the return statement.
     * @param[in] state Active control flow state with return requirements.
     */
    void CheckReturnStatement(const ControlFlowRecord& record, const ControlFlowState& state);

    /**
     * @brief Validates a function or lambda declaration for definite returns and updates state.
     * @param[in] record AST-free statement record for the function or lambda declaration.
     * @param[in,out] state Control flow state updated with function return expectations.
     */
    void CheckFunctionDeclaration(const ControlFlowRecord& record, ControlFlowState& state);

    /**
     * @brief Validates break and continue statements against loop and switch nesting depth.
     * @param[in] record AST-free statement record for break or continue.
     * @param[in] state Active control flow state with loop and switch depth.
     * @return True if the statement is a loop control statement and was handled; false otherwise.
     */
    bool CheckLoopControl(const ControlFlowRecord& record, const ControlFlowState& state);

    /**
     * @brief Updates loop or switch nesting depth and verifies branch structure.
     * @param[in] record AST-free statement record for branch or loop.
     * @param[in,out] state Control flow state updated with new loop or switch depth.
     */
    void UpdateBranchState(const ControlFlowRecord& record, ControlFlowState& state);

    /**
     * @brief Visits a control flow record, popping finished scopes and evaluating flow rules.
     * @param[in] record AST-free control flow statement record to evaluate.
     * @param[in,out] frames Stack of active lexical frames with their control flow states.
     */
    void Visit(const ControlFlowRecord& record, std::vector<std::pair<uint32_t, ControlFlowState>>& frames);

  private:
    std::string_view m_sourceCode;
    DiagnosticContext& m_ctx;
    const ControlFlowFacts& m_facts;
    std::deque<std::string> m_lambdaStrings;
};

} // namespace angel_lsp::analysis
