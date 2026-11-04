#include "analysis/checkers/null_safety/NullSafetyChecker.h"

#include "analysis/checkers/null_safety/NullSafetyCondition.h"
#include "analysis/checkers/null_safety/NullSafetyDecl.h"
#include "analysis/checkers/null_safety/NullSafetyExpr.h"
#include "analysis/checkers/null_safety/NullSafetyParam.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
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

    void Run(TSNode statement, FlowState& state)
    {
        m_pending.push_back({NullStatementAction::Statement, statement});
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
    void PushNextChild(SyntaxNodeId id)
    {
        const auto* child = m_ctx.index.SyntaxNode(id);
        while (child && !child->named)
        {
            child = m_ctx.index.SyntaxNode(child->next);
        }
        if (child)
        {
            m_pending.push_back({NullStatementAction::Children, {}, child->next});
            m_pending.push_back({NullStatementAction::Statement, m_ctx.index.SyntaxHandle(*child)});
        }
    }

    void BeginBranch(const SyntaxOperands& syntax, FlowState& state)
    {
        const auto& gs = parser::GetGrammarSymbols();
        const TSNode condition = syntax.FieldOrNamed(gs.fieldCondition, 0);
        NullConditionAssertions assertions;
        CheckNullExpression(condition, state, m_ctx, &assertions);
        auto& branch = m_branches.emplace_back();
        branch.elseState = state;
        ApplyAssertions(branch.elseState, assertions.negative);
        ApplyAssertions(state, assertions.positive);
        m_pending.push_back({NullStatementAction::Else, syntax.FieldOrNamed(gs.fieldAlternative, 2)});
        m_pending.push_back({NullStatementAction::Statement, syntax.FieldOrNamed(gs.fieldConsequence, 1)});
    }

    void StartElse(TSNode alternative, FlowState& state)
    {
        auto& branch = m_branches.back();
        branch.thenState = std::move(state);
        state = std::move(branch.elseState);
        m_pending.push_back({NullStatementAction::Join});
        m_pending.push_back({NullStatementAction::Statement, alternative});
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

    void CheckStatement(TSNode statement, FlowState& state)
    {
        if (state.isTerminated || ts_node_is_null(statement))
        {
            return;
        }
        const auto& gs = parser::GetGrammarSymbols();
        const auto symbol = ts_node_symbol(statement);
        if (symbol == gs.symStatementBlock)
        {
            if (const auto* block = m_ctx.index.FindOperandSyntax(statement))
            {
                PushNextChild(block->first);
            }
        }
        else if (symbol == gs.symVariableDeclaration)
        {
            CheckVarDeclaration(statement, state, m_ctx);
        }
        else if (symbol == gs.symExpressionStatement)
        {
            const TSNode expr = SyntaxOperands(statement, &m_ctx.index).FirstNamed();
            if (!ts_node_is_null(expr) && ts_node_symbol(expr) == gs.symAssignmentExpression)
            {
                CheckAssignmentStmt(expr, state, m_ctx);
            }
            else
            {
                CheckNullExpression(expr, state, m_ctx);
            }
        }
        else if (symbol == gs.symIfStatement)
        {
            BeginBranch(SyntaxOperands(statement, &m_ctx.index), state);
        }
        else if (symbol == gs.symReturnStatement)
        {
            CheckNullExpression(SyntaxOperands(statement, &m_ctx.index).Field(gs.fieldValue), state, m_ctx);
            state.isTerminated = true;
        }
    }

    NullCheckContext& m_ctx;
    std::vector<NullStatementWork> m_pending;
    std::vector<NullBranchState> m_branches;
};

static void AnalyzeFunction(TSNode funcNode, NullCheckContext& ctx)
{
    TSNode body = FindFunctionBody(funcNode, ctx.index);
    if (ts_node_is_null(body))
    {
        return;
    }

    FlowState state;
    CollectParameters(funcNode, body, ctx, state);
    NullStatementWorklist(ctx).Run(body, state);
}

void CheckNullSafety(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.rootNode) || request.sourceCode.empty() || !request.nodeIndex)
    {
        return;
    }
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    NullCheckContext checkCtx{request.sourceCode, ctx, request.scopeRoot, *request.nodeIndex};
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode funcNode : request.nodeIndex->Nodes(gs.symFuncDeclaration))
    {
        AnalyzeFunction(funcNode, checkCtx);
    }
    for (TSNode lambdaNode : request.nodeIndex->Nodes(gs.symLambdaExpression))
    {
        AnalyzeFunction(lambdaNode, checkCtx);
    }
}

} // namespace angel_lsp::analysis
