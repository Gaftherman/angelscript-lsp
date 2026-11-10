#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/checkers/DefiniteAssignmentChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"
#include "parser/GrammarNames.h"

#include <ankerl/unordered_dense.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string_view>
#include <tree_sitter/api.h>
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
     * @brief Constructs a visitor instance bound to the active check request and diagnostic sink.
     * @param[in] request AST check context containing source code and node index.
     * @param[in,out] ctx Diagnostic context for reporting uninitialized variable reads.
     */
    DefiniteAssignmentVisitor(const AstCheckContext& request, DiagnosticContext& ctx);

    /**
     * @brief Analyzes definite assignment dataflow across statements in a callable body.
     * @param[in] callable Syntax node ID of the function or lambda to analyze.
     */
    void AnalyzeFunction(SyntaxNodeId callable);

    /**
     * @brief Schedules a statement syntax node onto the worklist or evaluates an expression immediately.
     * @param[in] operand Syntax node representing a statement or expression.
     * @param[in,out] state Definite assignment state propagated into the node.
     */
    void Schedule(const IndexedSyntaxNode* operand, DefiniteAssignmentState& state);

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
     * @param[in] condition Tree-Sitter syntax node of the loop condition expression.
     * @param[in] isFor True if the enclosing loop is a for statement.
     */
    void MergeLoopBody(AssignmentWorkFrame& frame, TSNode condition, bool isFor);

    /**
     * @brief Steps through evaluation phases of a while or for loop statement.
     * @param[in,out] frame Worklist frame tracking loop initialization, condition, body, and update.
     * @param[in] symbol Grammar symbol of the loop statement (symWhileStatement or symForStatement).
     * @return True if loop processing is complete; false to continue stepping.
     */
    bool StepLoop(AssignmentWorkFrame& frame, TSSymbol symbol);

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
     * @brief Dispatches statement execution to the appropriate step handler based on node symbol.
     * @param[in,out] frame Worklist frame representing the active statement.
     * @return True if statement processing is complete and frame should be popped; false otherwise.
     */
    bool Step(AssignmentWorkFrame& frame);

    /**
     * @brief Executes terminal or simple leaf statements (return, break, continue, expression, var decl).
     * @param[in,out] frame Worklist frame holding the leaf statement.
     * @param[in] symbol Grammar symbol of the leaf statement.
     */
    void StepLeaf(AssignmentWorkFrame& frame, TSSymbol symbol);

    /**
     * @brief Extracts the simple identifier name from a direct or scoped identifier syntax node.
     * @param[in] operand Syntax node representing an identifier or scoped identifier.
     * @return Identifier name view, or an empty view if node is composite or non-identifier.
     */
    std::string_view SimpleIdentifierName(const IndexedSyntaxNode* operand) const;

    /**
     * @brief Checks if an identifier read references an uninitialized tracked local variable.
     * @param[in] operand Syntax node of the identifier expression.
     * @param[in] state Current definite assignment state.
     */
    void CheckIdentifierRead(const IndexedSyntaxNode* operand, const DefiniteAssignmentState& state);

    /**
     * @brief Marks a tracked local variable as assigned if the operand names one.
     * @param[in] operand Target operand of an assignment or out-parameter write.
     * @param[in,out] state Definite assignment state updated with the assigned variable.
     * @return True if operand was a tracked local variable and marked assigned; false otherwise.
     */
    bool MarkTrackedAssignment(const IndexedSyntaxNode* operand, DefiniteAssignmentState& state);

    /**
     * @brief Queues an operand for deferred expression read evaluation.
     * @param[in] operand Syntax node to be evaluated.
     * @param[in] kind Evaluation kind controlling read vs write-or-read handling.
     */
    void QueueRead(const IndexedSyntaxNode* operand, AssignmentReadKind kind = AssignmentReadKind::Read);

    /**
     * @brief Decomposes an assignment expression into left and right read/write tasks.
     * @param[in] node Assignment expression syntax node.
     */
    void QueueAssignmentReads(const IndexedSyntaxNode* node);

    /**
     * @brief Traverses an expression syntax node and enqueues sub-expressions for evaluation.
     * @param[in] node Root expression syntax node.
     * @param[in] state Current definite assignment state.
     */
    void QueueExpressionReads(const IndexedSyntaxNode* node, const DefiniteAssignmentState& state);

    /**
     * @brief Processes a single deferred read task from the evaluation queue.
     * @param[in] task Read task specifying operand and read kind.
     * @param[in,out] state Definite assignment state updated during evaluation.
     */
    void EvaluateReadTask(AssignmentReadTask task, DefiniteAssignmentState& state);

    /**
     * @brief Recursively evaluates expression reads for a node under the current assignment state.
     * @param[in] node Expression syntax node to check.
     * @param[in,out] state Definite assignment state updated during expression evaluation.
     */
    void CheckExpressionReads(const IndexedSyntaxNode* node, DefiniteAssignmentState& state);

    /**
     * @brief Analyzes a variable declarator node for initialization expressions or tracked primitive locals.
     * @param[in] node Variable declarator syntax node.
     * @param[in] isPrimitive True if the declaring type is an uninitialized primitive type.
     * @param[in,out] state Definite assignment state updated if initialized.
     */
    void AnalyzeVariableDeclarator(const IndexedSyntaxNode* node, bool isPrimitive, DefiniteAssignmentState& state);

    /**
     * @brief Analyzes a local variable declaration statement and registers its declarators.
     * @param[in] node Variable declaration statement syntax node.
     * @param[in,out] state Definite assignment state updated with initialized variables.
     */
    void AnalyzeVariableDeclaration(const IndexedSyntaxNode* node, DefiniteAssignmentState& state);

    /**
     * @brief Collects candidate function symbols matching the callee of a call expression.
     * @param[in] callee Callee syntax node (identifier or member expression).
     * @param[in] call Enclosing call expression syntax node.
     * @param[out] candidates Output vector populated with viable function symbols.
     */
    void CollectCallCandidates(const IndexedSyntaxNode* callee, const IndexedSyntaxNode* call,
                               std::vector<Symbol>& candidates) const;

    /**
     * @brief Collects argument syntax nodes from a call expression argument list.
     * @param[in] call Call expression syntax node.
     * @return Vector of syntax node pointers representing call arguments.
     */
    std::vector<const IndexedSyntaxNode*> CollectReadArguments(const IndexedSyntaxNode* call) const;

    /**
     * @brief Checks whether any argument in a call expression names an unassigned tracked local.
     * @param[in] arguments Call argument syntax nodes.
     * @param[in] state Current definite assignment state.
     * @return True if at least one argument is an unassigned tracked local; false otherwise.
     */
    bool HasAnyUnassignedTrackedLocal(const std::vector<const IndexedSyntaxNode*>& arguments,
                                      const DefiniteAssignmentState& state) const;

    /**
     * @brief Determines whether the i-th argument corresponds to an 'out' reference parameter.
     * @param[in] i Zero-based parameter position.
     * @param[in] info Selected signature and candidate symbols for the call.
     * @return True if the parameter at position i is an out parameter; false otherwise.
     */
    bool IsOutParameter(size_t i, const CallCandidateInfo& info) const;

    /**
     * @brief Resolves overloads and queues argument reads with appropriate read vs write-or-read kinds.
     * @param[in] call Call expression syntax node.
     * @param[in] arguments Ordered list of argument syntax nodes.
     */
    void QueueResolvedArgumentReads(const IndexedSyntaxNode* call,
                                    const std::vector<const IndexedSyntaxNode*>& arguments);

    /**
     * @brief Queues argument reads for a call expression, resolving overloads if unassigned locals are present.
     * @param[in] call Call expression syntax node.
     * @param[in] state Current definite assignment state.
     */
    void QueueCallArgumentReads(const IndexedSyntaxNode* call, const DefiniteAssignmentState& state);

    /**
     * @brief Retrieves the first child syntax node of the specified operand.
     * @param[in] node Parent syntax node.
     * @return Pointer to the first child IndexedSyntaxNode, or nullptr if none exists.
     */
    const IndexedSyntaxNode* First(const IndexedSyntaxNode* node) const
    {
        return node ? m_index.SyntaxNode(node->first) : nullptr;
    }

    /**
     * @brief Retrieves the next sibling syntax node of the specified operand.
     * @param[in] operand Current syntax node.
     * @return Pointer to the next sibling IndexedSyntaxNode, or nullptr if none exists.
     */
    const IndexedSyntaxNode* Next(const IndexedSyntaxNode* operand) const
    {
        return operand ? m_index.SyntaxNode(operand->next) : nullptr;
    }

    /**
     * @brief Finds the first child syntax node satisfying a predicate.
     * @tparam Predicate Callable type accepting a const IndexedSyntaxNode& and returning bool.
     * @param[in] node Parent syntax node.
     * @param[in] predicate Filtering predicate evaluated for each child.
     * @return Pointer to the matching IndexedSyntaxNode, or nullptr if none match.
     */
    template <typename Predicate>
    const IndexedSyntaxNode* FindOperand(const IndexedSyntaxNode* node, Predicate predicate) const
    {
        for (auto* operand = First(node); operand; operand = Next(operand))
        {
            if (predicate(*operand))
            {
                return operand;
            }
        }
        return nullptr;
    }

    /**
     * @brief Finds the child syntax node associated with a specific grammar field ID.
     * @param[in] node Parent syntax node.
     * @param[in] field Tree-Sitter grammar field identifier.
     * @return Pointer to the matching IndexedSyntaxNode, or nullptr if not found.
     */
    const IndexedSyntaxNode* FindField(const IndexedSyntaxNode* node, TSFieldId field) const
    {
        return field == 0
                   ? nullptr
                   : FindOperand(node, [field](const IndexedSyntaxNode& operand) { return operand.field == field; });
    }

    /**
     * @brief Finds the n-th named sibling syntax node starting from an initial operand.
     * @param[in] operand Starting syntax node.
     * @param[in] ordinal Zero-based index among named sibling nodes.
     * @return Pointer to the n-th named IndexedSyntaxNode, or nullptr if out of range.
     */
    const IndexedSyntaxNode* NthNamed(const IndexedSyntaxNode* operand, unsigned ordinal) const
    {
        for (; operand; operand = Next(operand))
        {
            if (operand->named && ordinal-- == 0)
            {
                return operand;
            }
        }
        return nullptr;
    }

    /**
     * @brief Finds a child syntax node by grammar field ID, falling back to ordinal named child.
     * @param[in] node Parent syntax node.
     * @param[in] field Tree-Sitter grammar field identifier.
     * @param[in] ordinal Fallback zero-based index among named children.
     * @return Pointer to the resolved IndexedSyntaxNode, or nullptr if not found.
     */
    const IndexedSyntaxNode* FindFieldOrNamed(const IndexedSyntaxNode* node, TSFieldId field,
                                              unsigned ordinal = 0) const
    {
        const IndexedSyntaxNode* operand = FindField(node, field);
        return operand ? operand : NthNamed(First(node), ordinal);
    }

    /**
     * @brief Obtains the underlying Tree-Sitter node handle for an indexed syntax node.
     * @param[in] operand Indexed syntax node.
     * @return Corresponding TSNode handle, or a null TSNode if operand is nullptr.
     */
    TSNode Syntax(const IndexedSyntaxNode* operand) const
    {
        return operand ? m_index.SyntaxHandle(*operand) : TSNode{};
    }

  private:
    /**
     * @brief AST check context carrying source buffer, syntax revision, and workspace symbols.
     */
    const AstCheckContext& m_request;

    /**
     * @brief Diagnostic sink where uninitialized variable read diagnostics are emitted.
     */
    DiagnosticContext& m_ctx;

    /**
     * @brief Pre-indexed syntax tree node cache for fast structural lookups.
     */
    const NodeIndex& m_index;

    /**
     * @brief LIFO worklist of control flow statements pending traversal.
     */
    std::deque<AssignmentWorkFrame> m_work;

    /**
     * @brief LIFO worklist of expression reads pending definite assignment evaluation.
     */
    std::vector<AssignmentReadTask> m_reads;

    /**
     * @brief Local variable names currently tracked for uninitialized read detection.
     */
    ankerl::unordered_dense::set<std::string_view> m_trackedLocals;

    /**
     * @brief Encoded row-column keys of AST locations where warnings have already been emitted.
     */
    ankerl::unordered_dense::set<uint64_t> m_reportedReads;
};

} // namespace angel_lsp::analysis
