#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/checkers/DefiniteAssignmentChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/DefiniteAssignmentFacts.h"
#include "analysis/index/Symbol.h"

#include <ankerl/unordered_dense.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Combines initialized names from viable branches according to AngelScript's union semantics.
 * @param[in] a Initialized variable names from the first branch.
 * @param[in] b Initialized variable names from the second branch.
 * @return Union set of initialized variable names from both branches.
 */
ankerl::unordered_dense::set<std::string_view>
MergeBranchStates(const ankerl::unordered_dense::set<std::string_view>& a,
                  const ankerl::unordered_dense::set<std::string_view>& b);

/**
 * @brief Evaluates extracted statement edges in execution order while tracking initialized locals.
 */
class DefiniteAssignmentVisitor
{
  public:
    /**
     * @brief Constructs a visitor instance bound to the active check request, diagnostic sink, and facts.
     * @param[in] request AST check context containing source code and scope tree.
     * @param[in,out] ctx Diagnostic context for reporting uninitialized variable reads.
     * @param[in] facts Precalculated pass-1 definite assignment facts.
     */
    DefiniteAssignmentVisitor(const AstCheckContext& request, DiagnosticContext& ctx,
                              const DefiniteAssignmentFacts& facts);

    /**
     * @brief Analyzes definite assignment dataflow across statements in a callable body.
     * @param[in] fn Pre-indexed function fact describing the callable body.
     */
    void AnalyzeFunction(const DefiniteAssignmentFunctionFact& fn);

    /**
     * @brief Schedules a statement record onto the worklist or evaluates a leaf statement immediately.
     * @param[in] recordIndex Index of the statement record in facts.
     * @param[in,out] state Definite assignment state propagated into the node.
     */
    void Schedule(uint32_t recordIndex, DefiniteAssignmentState& state);

    /**
     * @brief Merges definite assignment states from then and else branches of an if statement.
     * @param[in,out] frame Worklist frame holding condition, branch, and alternate states.
     */
    static void MergeIfBranches(AssignmentWorkFrame& frame);

    /**
     * @brief Steps through evaluation phases of an if statement.
     * @param[in,out] frame Worklist frame tracking if condition and branches.
     * @return True if processing of this statement is complete; false to continue stepping.
     */
    bool StepIf(AssignmentWorkFrame& frame);

    /**
     * @brief Steps through statements within a statement block or case clause.
     * @param[in,out] frame Worklist frame tracking statements in the block.
     * @param[in] clause True if the block is a switch case clause.
     * @return True if the block traversal is finished; false to continue stepping.
     */
    bool StepBlock(AssignmentWorkFrame& frame, bool clause);

    /**
     * @brief Merges loop body definite assignment state back into the enclosing loop state.
     * @param[in,out] frame Worklist frame holding loop body branch state.
     * @param[in] conditionAlwaysTrue True if loop condition is statically known to be always true.
     */
    void MergeLoopBody(AssignmentWorkFrame& frame, bool conditionAlwaysTrue);

    /**
     * @brief Steps through evaluation phases of a while or for loop statement.
     * @param[in,out] frame Worklist frame tracking loop initialization, condition, body, and update.
     * @param[in] isFor True if the statement is a for statement; false for while.
     * @return True if loop processing is complete; false to continue stepping.
     */
    bool StepLoop(AssignmentWorkFrame& frame, bool isFor);

    /**
     * @brief Steps through evaluation phases of a do-while loop statement.
     * @param[in,out] frame Worklist frame tracking do-while body and condition.
     * @return True if do-while processing is complete; false to continue stepping.
     */
    bool StepDoWhile(AssignmentWorkFrame& frame);

    /**
     * @brief Steps through evaluation phases of a switch statement and its case clauses.
     * @param[in,out] frame Worklist frame tracking switch condition and case clauses.
     * @return True if switch statement processing is complete; false to continue stepping.
     */
    bool StepSwitch(AssignmentWorkFrame& frame);

    /**
     * @brief Dispatches statement execution to the appropriate step handler based on statement kind.
     * @param[in,out] frame Worklist frame representing the active statement.
     * @return True if statement processing is complete and frame should be popped; false otherwise.
     */
    bool Step(AssignmentWorkFrame& frame);

    /**
     * @brief Executes terminal or simple leaf statements (expression, variable declaration, return, break).
     * @param[in] rec Statement record.
     * @param[in,out] state Active definite assignment state.
     */
    void StepLeaf(const DefiniteAssignmentStmtRecord& rec, DefiniteAssignmentState& state);

    /**
     * @brief Evaluates expressions, reads, assignments, and calls associated with a statement record.
     * @param[in] rec Statement record whose expressions are evaluated.
     * @param[in,out] state Active definite assignment state updated during evaluation.
     */
    void EvaluateStatementExpressions(const DefiniteAssignmentStmtRecord& rec, DefiniteAssignmentState& state);

    /**
     * @brief Collects candidate function symbols matching the callee of a call expression.
     * @param[in] call Call fact containing callee and receiver details.
     * @param[out] candidates Output vector populated with viable function symbols.
     */
    void CollectCallCandidates(const DefiniteAssignmentCallFact& call, std::vector<Symbol>& candidates) const;

    /**
     * @brief Checks whether any argument in a call expression names an unassigned tracked local.
     * @param[in] arguments Call argument facts.
     * @param[in] state Current definite assignment state.
     * @return True if at least one argument is an unassigned tracked local; false otherwise.
     */
    bool HasAnyUnassignedTrackedLocal(std::span<const DefiniteAssignmentArgumentFact> arguments,
                                      const DefiniteAssignmentState& state) const;

    /**
     * @brief Determines whether the i-th argument corresponds to an 'out' reference parameter.
     * @param[in] i Zero-based parameter position.
     * @param[in] info Selected signature and candidate symbols for the call.
     * @return True if the parameter at position i is an out parameter; false otherwise.
     */
    bool IsOutParameter(size_t i, const CallCandidateInfo& info) const;

    /**
     * @brief Processes a call expression, resolving out parameters to mark tracked arguments assigned.
     * @param[in] call Call fact to evaluate.
     * @param[in,out] state Definite assignment state updated if out parameters assign tracked locals.
     */
    void ProcessCall(const DefiniteAssignmentCallFact& call, DefiniteAssignmentState& state);

  private:
    const AstCheckContext& m_request;
    DiagnosticContext& m_ctx;
    const DefiniteAssignmentFacts& m_facts;
    std::deque<AssignmentWorkFrame> m_work;
    ankerl::unordered_dense::set<std::string_view> m_trackedLocals;
    ankerl::unordered_dense::set<uint64_t> m_reportedReads;
};

} // namespace angel_lsp::analysis
