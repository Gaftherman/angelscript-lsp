#pragma once

#include "analysis/checkers/ControlFlowChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "parser/GrammarNames.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Validates case labels within a switch statement for duplicate values and constantness.
 * @param[in] labels Pre-indexed or extracted case label syntax nodes.
 * @param[in] sourceCode Source text of the document under analysis.
 * @param[in,out] ctx Diagnostic collection context for emitting invalid or duplicate case errors.
 */
void CheckSwitchLabels(std::span<const TSNode> labels, std::string_view sourceCode, DiagnosticContext& ctx);

/**
 * @brief Structure holding state and parameters for ControlFlowVisitor.
 */
class ControlFlowVisitor
{
  public:
    /**
     * @brief Constructs a visitor instance bound to the active source, diagnostic sink, and node index.
     * @param[in] sourceCode Document source text.
     * @param[in,out] ctx Diagnostic collection context.
     * @param[in] index Syntax node index for looking up nodes and flow facts.
     */
    ControlFlowVisitor(std::string_view sourceCode, DiagnosticContext& ctx, const NodeIndex& index);

    /**
     * @brief Runs control-flow validation across document statements in ordered sequence.
     */
    void Run();

    /**
     * @brief Check return statement.
     * @param[in] node AST node under evaluation.
     * @param[in] state Active control flow state with return requirements.
     */
    void CheckReturnStatement(TSNode node, const ControlFlowState& state);

    /**
     * @brief Validates a function or lambda declaration for definite returns and updates state.
     * @param[in] node AST function or lambda declaration node.
     * @param[in,out] state Control flow state updated with function return expectations.
     */
    void CheckFunctionDeclaration(TSNode node, ControlFlowState& state);

    /**
     * @brief Validates break and continue statements against loop and switch nesting depth.
     * @param[in] node AST break or continue statement node.
     * @param[in] sym Grammar symbol of the statement.
     * @param[in] state Active control flow state with loop and switch depth.
     * @return True if the statement is a loop control statement and was handled; false otherwise.
     */
    bool CheckLoopControl(TSNode node, TSSymbol sym, const ControlFlowState& state);

    /**
     * @brief Updates loop or switch nesting depth and verifies branch structure.
     * @param[in] node AST branch or loop statement node.
     * @param[in] sym Grammar symbol of the statement.
     * @param[in,out] state Control flow state updated with new loop or switch depth.
     */
    void UpdateBranchState(TSNode node, TSSymbol sym, ControlFlowState& state);

    /**
     * @brief Visits an AST statement node, popping finished scopes and evaluating flow rules.
     * @param[in] node AST statement node to evaluate.
     * @param[in,out] frames Stack of active lexical frames with their control flow states.
     */
    void Visit(TSNode node, std::vector<std::pair<uint32_t, ControlFlowState>>& frames);

  private:
    std::string_view m_sourceCode;
    DiagnosticContext& m_ctx;
    const NodeIndex& m_index;
};

} // namespace angel_lsp::analysis
