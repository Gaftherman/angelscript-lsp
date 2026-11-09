#include "analysis/checkers/null_safety/NullSafetyExpr.h"

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/checkers/null_safety/NullSafetyCondition.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
static void CheckMemberDereference(TSNode obj, FlowState& state, NullCheckContext& ctx)
{
    if (ts_node_is_null(obj))
    {
        return;
    }
    std::string name = GetIdentifierName(obj, ctx);
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
                ctx.diagCtx.EmitAtRange(ToSourceRange(obj), diagnostics::codes::PossibleNullDereference, name,
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
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol objSym = ts_node_symbol(obj);
    if (objSym == gs.symCastExpression)
    {
        TSNode typeNode = SyntaxOperands(obj, &ctx.index).Field(gs.fieldType);
        const auto typeInfo = ExtractTypeInfoFromAST(typeNode, ctx.sourceCode, &ctx.index);
        if (typeInfo.isHandle)
        {
            std::string typeText = parser::NodeText(typeNode, ctx.sourceCode);
            ctx.diagCtx.EmitAtRange(ToSourceRange(obj), diagnostics::codes::PossibleNullDereference, typeText,
                                    DiagnosticSeverity::Warning);
        }
    }
}

static void DispatchNonBinaryExpr(TSNode expr, std::vector<ExprStackFrame>& stack, FlowState& state,
                                  NullCheckContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto sym = ts_node_symbol(expr);
    const SyntaxOperands syntax(expr, &ctx.index);
    if (sym == gs.symMemberExpression)
    {
        TSNode obj = syntax.Field(gs.fieldObject);
        CheckMemberDereference(obj, state, ctx);
        stack.push_back({ExprActionKind::Eval, obj});
    }
    else if (sym == gs.symIndexExpression)
    {
        TSNode obj = syntax.Field(gs.fieldObject);
        CheckMemberDereference(obj, state, ctx);
        stack.push_back({ExprActionKind::Eval, syntax.Field(gs.fieldIndex)});
        stack.push_back({ExprActionKind::Eval, obj});
    }
    else if (sym == gs.symCallExpression)
    {
        TSNode argsNode = syntax.Field(gs.fieldArguments);
        const auto firstArgument = stack.size();
        SyntaxOperands(argsNode, &ctx.index)
            .ForEachNamed(
                [&](TSNode argument)
                {
                    stack.push_back({ExprActionKind::Eval, argument});
                    return true;
                });
        std::ranges::reverse(std::span(stack).subspan(firstArgument));
        stack.push_back({ExprActionKind::Eval, syntax.Field(gs.fieldFunction)});
    }
    else if (sym == gs.symCastExpression)
    {
        stack.push_back({ExprActionKind::Eval, syntax.Field(gs.fieldValue)});
    }
    else if (sym == gs.symParenthesizedExpression)
    {
        stack.push_back({ExprActionKind::Eval, syntax.FirstNamed()});
    }
    else if (sym == gs.symTernaryExpression)
    {
        stack.push_back({ExprActionKind::Eval, syntax.Field(gs.fieldAlternative)});
        stack.push_back({ExprActionKind::Eval, syntax.Field(gs.fieldConsequence)});
        stack.push_back({ExprActionKind::Eval, syntax.Field(gs.fieldCondition)});
    }
}

static void DispatchBinaryExpr(TSNode expr, std::vector<ExprStackFrame>& stack, const NodeIndex& index)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto [op, left, right] = ReadBinaryOperands(expr, &index);
    const TSSymbol opSym = ts_node_is_null(op) ? 0 : ts_node_symbol(op);

    if (opSym == gs.tokAmpAmp || opSym == gs.tokAnd)
    {
        stack.push_back({ExprActionKind::AfterLeftAnd, left, right});
        stack.push_back({ExprActionKind::Eval, left});
    }
    else if (opSym == gs.tokPipePipe || opSym == gs.tokOr)
    {
        stack.push_back({ExprActionKind::AfterLeftOr, left, right});
        stack.push_back({ExprActionKind::Eval, left});
    }
    else
    {
        stack.push_back({ExprActionKind::Eval, right});
        stack.push_back({ExprActionKind::Eval, left});
    }
}

static void ProcessAfterLeft(const ExprStackFrame& frame, std::vector<ExprStackFrame>& stack, FlowState& state,
                             NullConditionWorklist& conditions)
{
    const auto& assertions = conditions.Remember(frame.node);
    auto savedVars = state.vars;
    ApplyAssertions(state, frame.kind == ExprActionKind::AfterLeftAnd ? assertions.positive : assertions.negative);

    stack.push_back({ExprActionKind::RestoreVars, {}, {}, std::move(savedVars)});
    stack.push_back({ExprActionKind::Eval, frame.right});
}

void CheckNullExpression(TSNode expr, FlowState& state, NullCheckContext& ctx, NullConditionAssertions* assertions)
{
    if (ts_node_is_null(expr))
    {
        if (assertions)
        {
            *assertions = {};
        }
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
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

        TSNode node = frame.node;
        if (ts_node_is_null(node))
        {
            continue;
        }

        const TSSymbol sym = ts_node_symbol(node);
        if (sym == gs.symBinaryExpression)
        {
            DispatchBinaryExpr(node, stack, ctx.index);
        }
        else
        {
            DispatchNonBinaryExpr(node, stack, state, ctx);
        }
    }
    if (assertions)
    {
        *assertions = conditions.Take(expr);
    }
}

} // namespace angel_lsp::analysis
