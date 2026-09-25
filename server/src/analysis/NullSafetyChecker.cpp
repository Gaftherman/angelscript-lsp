#include "analysis/NullSafetyChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/NodeIndex.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/NullSafetyDecl.h"
#include "analysis/NullSafetyExpr.h"
#include "analysis/NullSafetyParam.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"
#include <string>

namespace angel_lsp::analysis
{
namespace
{
std::string NodeText(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return "";
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return "";
    }
    return std::string(sourceCode.substr(start, end - start));
}

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

void CheckStatement(TSNode stmt, FlowState& state, NullCheckContext& ctx, int depth);

void CheckIfStmt(TSNode stmt, FlowState& state, NullCheckContext& ctx, int depth)
{
    TSNode cond = parser::GetChildByField(stmt, parser::fields::Condition);
    if (ts_node_is_null(cond))
    {
        cond = ts_node_named_child(stmt, 0);
    }
    TSNode conseq = parser::GetChildByField(stmt, parser::fields::Consequence);
    if (ts_node_is_null(conseq) && ts_node_named_child_count(stmt) > 1)
    {
        conseq = ts_node_named_child(stmt, 1);
    }
    TSNode alt = parser::GetChildByField(stmt, parser::fields::Alternative);
    if (ts_node_is_null(alt) && ts_node_named_child_count(stmt) > 2)
    {
        alt = ts_node_named_child(stmt, 2);
    }

    CheckNullExpression(cond, state, ctx, depth + 1);
    std::vector<NullAssertion> posAssertions;
    std::vector<NullAssertion> negAssertions;
    ExtractConditionAssertions(cond, ctx.sourceCode, posAssertions, negAssertions);

    FlowState thenState = state;
    ApplyAssertions(thenState, posAssertions);
    CheckStatement(conseq, thenState, ctx, depth + 1);

    FlowState elseState = state;
    ApplyAssertions(elseState, negAssertions);

    if (!ts_node_is_null(alt))
    {
        CheckStatement(alt, elseState, ctx, depth + 1);
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

void CheckStatement(TSNode stmt, FlowState& state, NullCheckContext& ctx, int depth)
{
    if (ts_node_is_null(stmt) || state.isTerminated || depth > k_maxAstDepth)
    {
        return;
    }
    std::string_view stmtType = ts_node_type(stmt);
    if (stmtType == parser::nodes::StatementBlock)
    {
        const uint32_t count = ts_node_named_child_count(stmt);
        for (uint32_t i = 0; i < count && !state.isTerminated; ++i)
        {
            CheckStatement(ts_node_named_child(stmt, i), state, ctx, depth + 1);
        }
    }
    else if (stmtType == parser::nodes::VariableDeclaration)
    {
        CheckVarDeclaration(stmt, state, ctx);
    }
    else if (stmtType == parser::nodes::ExpressionStatement)
    {
        TSNode expr = ts_node_named_child(stmt, 0);
        if (!ts_node_is_null(expr) && std::string_view(ts_node_type(expr)) == parser::nodes::AssignmentExpression)
        {
            CheckAssignmentStmt(expr, state, ctx);
        }
        else
        {
            CheckNullExpression(expr, state, ctx, depth + 1);
        }
    }
    else if (stmtType == parser::nodes::IfStatement)
    {
        CheckIfStmt(stmt, state, ctx, depth + 1);
    }
    else if (stmtType == parser::nodes::ReturnStatement)
    {
        CheckNullExpression(parser::GetChildByField(stmt, parser::fields::Value), state, ctx, depth + 1);
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
    CheckStatement(body, state, ctx, 0);
}
} // namespace

void CheckNullSafety(const NullSafetyCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    NullCheckContext checkCtx{request.sourceCode, ctx};
    if (request.nodeIndex)
    {
        for (TSNode funcNode : request.nodeIndex->Nodes(parser::nodes::FuncDeclaration))
        {
            AnalyzeFunction(funcNode, request.scopeRoot, checkCtx);
        }
        for (TSNode lambdaNode : request.nodeIndex->Nodes(parser::nodes::LambdaExpression))
        {
            AnalyzeFunction(lambdaNode, request.scopeRoot, checkCtx);
        }
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(request.root);
    bool reachedRoot = false;
    while (!reachedRoot)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        std::string_view type = ts_node_type(node);
        if (type == parser::nodes::FuncDeclaration || type == parser::nodes::LambdaExpression)
        {
            AnalyzeFunction(node, request.scopeRoot, checkCtx);
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }
        while (!reachedRoot)
        {
            if (!ts_tree_cursor_goto_parent(&cursor))
            {
                reachedRoot = true;
                break;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                break;
            }
        }
    }
    ts_tree_cursor_delete(&cursor);
}

} // namespace angel_lsp::analysis
