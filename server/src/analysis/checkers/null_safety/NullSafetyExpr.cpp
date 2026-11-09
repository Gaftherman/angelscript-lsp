#include "analysis/checkers/null_safety/NullSafetyExpr.h"

#include "analysis/checkers/null_safety/NullSafetyCondition.h"
#include "analysis/diagnostics/DiagnosticCodes.h"

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
static void CheckMemberDereference(NullFlowNodeId obj, FlowState& state, NullCheckContext& ctx)
{
    if (obj == NoNullFlowNode)
    {
        return;
    }
    const NullFlowNode* node = ctx.facts.Find(obj);
    if (!node)
    {
        return;
    }
    const std::string_view name = GetIdentifierName(obj, ctx);
    if (!name.empty())
    {
        auto it = state.vars.find(name);
        if (it != state.vars.end() &&
            (it->second == Nullability::Nullable || it->second == Nullability::DefinitelyNull))
        {
            const bool reportAll =
                ctx.diagCtx.request.diagnostics && ctx.diagCtx.request.diagnostics->reportAllNullDereferences;
            if (reportAll || !state.warnedVars.contains(name))
            {
                ctx.diagCtx.EmitAtRange(node->range, diagnostics::codes::PossibleNullDereference, name,
                                        DiagnosticSeverity::Warning);
                state.warnedVars.insert(name);
            }
            if (!reportAll)
            {
                it->second = Nullability::NonNull;
            }
        }
        return;
    }
    if (node->kind == NullFlowNodeKind::Cast && node->typeIsHandle)
    {
        ctx.diagCtx.EmitAtRange(node->range, diagnostics::codes::PossibleNullDereference, node->typeText,
                                DiagnosticSeverity::Warning);
    }
}

static void DispatchNonBinaryExpr(const NullFlowNode& node, std::vector<ExprStackFrame>& stack, FlowState& state,
                                  NullCheckContext& ctx)
{
    if (node.kind == NullFlowNodeKind::Member)
    {
        const NullFlowNodeId obj = node.first;
        CheckMemberDereference(obj, state, ctx);
        stack.push_back({ExprActionKind::Eval, obj});
    }
    else if (node.kind == NullFlowNodeKind::Index)
    {
        const NullFlowNodeId obj = node.first;
        CheckMemberDereference(obj, state, ctx);
        stack.push_back({ExprActionKind::Eval, node.second});
        stack.push_back({ExprActionKind::Eval, obj});
    }
    else if (node.kind == NullFlowNodeKind::Call)
    {
        const auto firstArgument = stack.size();
        NullFlowNodeId argId = node.second;
        while (argId != NoNullFlowNode)
        {
            stack.push_back({ExprActionKind::Eval, argId});
            const NullFlowNode* argNode = ctx.facts.Find(argId);
            argId = argNode ? argNode->next : NoNullFlowNode;
        }
        std::ranges::reverse(std::span(stack).subspan(firstArgument));
        stack.push_back({ExprActionKind::Eval, node.first});
    }
    else if (node.kind == NullFlowNodeKind::Cast)
    {
        stack.push_back({ExprActionKind::Eval, node.first});
    }
    else if (node.kind == NullFlowNodeKind::Parenthesized)
    {
        stack.push_back({ExprActionKind::Eval, node.first});
    }
    else if (node.kind == NullFlowNodeKind::Ternary)
    {
        stack.push_back({ExprActionKind::Eval, node.third});
        stack.push_back({ExprActionKind::Eval, node.second});
        stack.push_back({ExprActionKind::Eval, node.first});
    }
}

static void DispatchBinaryExpr(const NullFlowNode& node, std::vector<ExprStackFrame>& stack)
{
    if (node.op == NullFlowOperator::And)
    {
        stack.push_back({ExprActionKind::AfterLeftAnd, node.first, node.second});
        stack.push_back({ExprActionKind::Eval, node.first});
    }
    else if (node.op == NullFlowOperator::Or)
    {
        stack.push_back({ExprActionKind::AfterLeftOr, node.first, node.second});
        stack.push_back({ExprActionKind::Eval, node.first});
    }
    else
    {
        stack.push_back({ExprActionKind::Eval, node.second});
        stack.push_back({ExprActionKind::Eval, node.first});
    }
}

static void ProcessAfterLeft(const ExprStackFrame& frame, std::vector<ExprStackFrame>& stack, FlowState& state,
                             NullConditionWorklist& conditions)
{
    const auto& assertions = conditions.Remember(frame.node);
    auto savedVars = state.vars;
    ApplyAssertions(state, frame.kind == ExprActionKind::AfterLeftAnd ? assertions.positive : assertions.negative);

    stack.push_back({ExprActionKind::RestoreVars, NoNullFlowNode, NoNullFlowNode, std::move(savedVars)});
    stack.push_back({ExprActionKind::Eval, frame.right});
}

/**
 * @brief Checks expressions for potential null handle dereferences.
 * @param[in] expr Expression node id to check.
 * @param[in,out] state Dataflow state tracking handle nullability.
 * @param[in,out] ctx Diagnostic context and source code view.
 * @param[out] assertions Facts for the whole expression when requested.
 */
void CheckNullExpression(NullFlowNodeId expr, FlowState& state, NullCheckContext& ctx,
                         NullConditionAssertions* assertions)
{
    if (expr == NoNullFlowNode)
    {
        if (assertions)
        {
            *assertions = {};
        }
        return;
    }
    NullConditionWorklist conditions(ctx);
    std::vector<ExprStackFrame> stack;
    stack.push_back({ExprActionKind::Eval, expr});

    while (!stack.empty())
    {
        ExprStackFrame frame = std::move(stack.back());
        stack.pop_back();

        if (frame.kind == ExprActionKind::RestoreVars)
        {
            state.vars = std::move(frame.savedVars);
            continue;
        }
        if (frame.kind == ExprActionKind::AfterLeftAnd || frame.kind == ExprActionKind::AfterLeftOr)
        {
            ProcessAfterLeft(frame, stack, state, conditions);
            continue;
        }

        const NullFlowNodeId nodeId = frame.node;
        if (nodeId == NoNullFlowNode)
        {
            continue;
        }

        const NullFlowNode* node = ctx.facts.Find(nodeId);
        if (!node)
        {
            continue;
        }

        if (node->kind == NullFlowNodeKind::Binary)
        {
            DispatchBinaryExpr(*node, stack);
        }
        else
        {
            DispatchNonBinaryExpr(*node, stack, state, ctx);
        }
    }
    if (assertions)
    {
        *assertions = conditions.Take(expr);
    }
}

} // namespace angel_lsp::analysis
