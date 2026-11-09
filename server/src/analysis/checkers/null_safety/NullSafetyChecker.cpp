#include "analysis/checkers/null_safety/NullSafetyChecker.h"

#include "analysis/checkers/null_safety/NullSafetyCondition.h"
#include "analysis/checkers/null_safety/NullSafetyDecl.h"
#include "analysis/checkers/null_safety/NullSafetyExpr.h"
#include "analysis/checkers/null_safety/NullSafetyParam.h"
#include "analysis/index/NullFlowFacts.h"
#include "utils/Utils.h"

#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
static FlowState MergeFlowStates(const FlowState& s1, const FlowState& s2)
{
    FlowState result;
    result.warnedVars = s1.warnedVars;
    result.warnedVars.insert(s2.warnedVars.begin(), s2.warnedVars.end());

    for (const auto& [var, val1] : s1.vars)
    {
        auto it2 = s2.vars.find(var);
        if (it2 != s2.vars.end())
        {
            result.vars[var] = (val1 == Nullability::NonNull && it2->second == Nullability::NonNull)
                                   ? Nullability::NonNull
                                   : Nullability::Nullable;
        }
        else
        {
            result.vars[var] = val1;
        }
    }
    return result;
}

/** @brief Evaluates captured statements in order and joins the states of each branch. */
class NullStatementWorklist
{
  public:
    explicit NullStatementWorklist(NullCheckContext& ctx) : m_ctx(ctx)
    {
    }

    void Run(NullFlowNodeId statement, FlowState& state)
    {
        m_pending.push_back({NullStatementAction::Statement, statement, NoNullFlowNode});
        while (!m_pending.empty())
        {
            const NullStatementWork work = m_pending.back();
            m_pending.pop_back();
            switch (work.action)
            {
            case NullStatementAction::Statement:
                CheckStatement(work.node, state);
                break;
            case NullStatementAction::Children:
                if (!state.isTerminated)
                {
                    PushNextChild(work.child);
                }
                break;
            case NullStatementAction::Else:
                StartElse(work.node, state);
                break;
            case NullStatementAction::Join:
                Join(state);
                break;
            }
        }
    }

  private:
    void PushNextChild(NullFlowNodeId id)
    {
        const auto* child = m_ctx.facts.Find(id);
        if (child)
        {
            m_pending.push_back({NullStatementAction::Children, NoNullFlowNode, child->next});
            m_pending.push_back({NullStatementAction::Statement, id, NoNullFlowNode});
        }
    }

    void BeginBranch(const NullFlowNode& node, FlowState& state)
    {
        const NullFlowNodeId condition = node.first;
        NullConditionAssertions assertions;
        CheckNullExpression(condition, state, m_ctx, &assertions);
        auto& branch = m_branches.emplace_back();
        branch.elseState = state;
        ApplyAssertions(branch.elseState, assertions.negative);
        ApplyAssertions(state, assertions.positive);
        m_pending.push_back({NullStatementAction::Else, node.third, NoNullFlowNode});
        m_pending.push_back({NullStatementAction::Statement, node.second, NoNullFlowNode});
    }

    void StartElse(NullFlowNodeId alternative, FlowState& state)
    {
        auto& branch = m_branches.back();
        branch.thenState = std::move(state);
        state = std::move(branch.elseState);
        m_pending.push_back({NullStatementAction::Join, NoNullFlowNode, NoNullFlowNode});
        m_pending.push_back({NullStatementAction::Statement, alternative, NoNullFlowNode});
    }

    void Join(FlowState& state)
    {
        auto& thenState = m_branches.back().thenState;
        if (!thenState.isTerminated)
        {
            state = state.isTerminated ? std::move(thenState) : MergeFlowStates(thenState, state);
        }
        m_branches.pop_back();
    }

    void CheckStatement(NullFlowNodeId id, FlowState& state)
    {
        if (state.isTerminated || id == NoNullFlowNode)
        {
            return;
        }
        const auto* node = m_ctx.facts.Find(id);
        if (!node)
        {
            return;
        }
        switch (node->kind)
        {
        case NullFlowNodeKind::Block:
            PushNextChild(node->first);
            break;
        case NullFlowNodeKind::VariableDeclaration:
            CheckVarDeclaration(id, state, m_ctx);
            break;
        case NullFlowNodeKind::ExpressionStatement:
        {
            const NullFlowNodeId expr = node->first;
            const auto* exprNode = m_ctx.facts.Find(expr);
            if (exprNode && exprNode->kind == NullFlowNodeKind::Assignment)
            {
                CheckAssignmentStmt(expr, state, m_ctx);
            }
            else
            {
                CheckNullExpression(expr, state, m_ctx);
            }
            break;
        }
        case NullFlowNodeKind::IfStatement:
            BeginBranch(*node, state);
            break;
        case NullFlowNodeKind::ReturnStatement:
            CheckNullExpression(node->first, state, m_ctx);
            state.isTerminated = true;
            break;
        default:
            break;
        }
    }

    NullCheckContext& m_ctx;
    std::vector<NullStatementWork> m_pending;
    std::vector<NullBranchState> m_branches;
};

static void AnalyzeFunction(const NullFlowCallable& callable, NullCheckContext& ctx)
{
    FlowState state;
    CollectParameters(callable, ctx, state);
    NullStatementWorklist(ctx).Run(callable.body, state);
}

/**
 * @brief Analyzes dataflow in functions and lambdas to detect dereferences of null or unchecked handles.
 * @param[in] request Document AST root, source text, and scope tree.
 * @param[in,out] ctx Diagnostic sink for emitting warnings.
 */
void CheckNullSafety(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || !request.nodeIndex)
    {
        return;
    }
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    const NullFlowFacts facts = BuildNullFlowFacts(*request.nodeIndex, request.sourceCode);
    NullCheckContext checkCtx{request.sourceCode, ctx, request.scopeRoot, facts};
    for (const auto& callable : facts.callables)
    {
        AnalyzeFunction(callable, checkCtx);
    }
}

} // namespace angel_lsp::analysis
