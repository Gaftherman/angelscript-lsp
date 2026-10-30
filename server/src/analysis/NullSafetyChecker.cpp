#include "analysis/NullSafetyChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/NodeIndex.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/NullSafetyDecl.h"
#include "analysis/NullSafetyExpr.h"
#include "analysis/NullSafetyParam.h"
#include "analysis/ast/SemanticNodes.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"
#include <string>

namespace angel_lsp::analysis
{
namespace
{
void ApplyAssertions(FlowState& state, const std::vector<NullAssertion>& assertions)
{
    for (const auto& a : assertions)
    {
        state.vars[a.varName] = a.state;
    }
}

FlowState MergeFlowStates(const FlowState& s1, const FlowState& s2)
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

void CheckStatement(TSNode stmt, FlowState& state, NullCheckContext& ctx);

void CheckIfStmt(TSNode stmt, FlowState& state, NullCheckContext& ctx)
{
    const ast::IfStatementView ifView(stmt, ctx.sourceCode);
    const TSNode cond = ifView.Condition();
    const TSNode conseq = ifView.Consequence();
    const TSNode alt = ifView.Alternative();

    CheckNullExpression(cond, state, ctx);
    std::vector<NullAssertion> posAssertions;
    std::vector<NullAssertion> negAssertions;
    ExtractConditionAssertions(cond, ctx.sourceCode, posAssertions, negAssertions);

    FlowState thenState = state;
    ApplyAssertions(thenState, posAssertions);
    CheckStatement(conseq, thenState, ctx);

    FlowState elseState = state;
    ApplyAssertions(elseState, negAssertions);

    if (ifView.HasAlternative())
    {
        CheckStatement(alt, elseState, ctx);
        if (thenState.isTerminated && elseState.isTerminated)
        {
            state.isTerminated = true;
        }
        else if (thenState.isTerminated)
        {
            state = elseState;
        }
        else if (elseState.isTerminated)
        {
            state = thenState;
        }
        else
        {
            state = MergeFlowStates(thenState, elseState);
        }
    }
    else
    {
        state = thenState.isTerminated ? elseState : MergeFlowStates(thenState, elseState);
    }
}

void CheckStatement(TSNode stmt, FlowState& state, NullCheckContext& ctx)
{
    if (ts_node_is_null(stmt) || state.isTerminated)
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol stmtSym = ts_node_symbol(stmt);
    if (stmtSym == gs.symStatementBlock)
    {
        const uint32_t count = ts_node_named_child_count(stmt);
        for (uint32_t i = 0; i < count && !state.isTerminated; ++i)
        {
            CheckStatement(ts_node_named_child(stmt, i), state, ctx);
        }
    }
    else if (stmtSym == gs.symVariableDeclaration)
    {
        CheckVarDeclaration(stmt, state, ctx);
    }
    else if (stmtSym == gs.symExpressionStatement)
    {
        TSNode expr = ts_node_named_child(stmt, 0);
        if (!ts_node_is_null(expr) && ts_node_symbol(expr) == gs.symAssignmentExpression)
        {
            CheckAssignmentStmt(expr, state, ctx);
        }
        else
        {
            CheckNullExpression(expr, state, ctx);
        }
    }
    else if (stmtSym == gs.symIfStatement)
    {
        CheckIfStmt(stmt, state, ctx);
    }
    else if (stmtSym == gs.symReturnStatement)
    {
        const ast::ReturnStatementView retView(stmt, ctx.sourceCode);
        CheckNullExpression(retView.Value(), state, ctx);
        state.isTerminated = true;
    }
}

void AnalyzeFunction(TSNode funcNode, const Scope* scopeRoot, NullCheckContext& ctx)
{
    TSNode body = FindFunctionBody(funcNode);
    if (ts_node_is_null(body))
    {
        return;
    }

    FlowState state;
    CollectParameters(funcNode, scopeRoot, ctx.sourceCode, state);
    CheckStatement(body, state, ctx);
}
} // namespace

void CheckNullSafety(const NullSafetyCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty() || !request.nodeIndex)
    {
        return;
    }
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    NullCheckContext checkCtx{request.sourceCode, ctx, request.scopeRoot};
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode funcNode : request.nodeIndex->Nodes(gs.symFuncDeclaration))
    {
        AnalyzeFunction(funcNode, request.scopeRoot, checkCtx);
    }
    for (TSNode lambdaNode : request.nodeIndex->Nodes(gs.symLambdaExpression))
    {
        AnalyzeFunction(lambdaNode, request.scopeRoot, checkCtx);
    }
}

} // namespace angel_lsp::analysis
