#include "analysis/NullSafetyExpr.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/SymbolTable.h"
#include "analysis/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include <memory>
#include <string>
#include <vector>

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

void CheckMemberDereference(TSNode obj, FlowState& state, NullCheckContext& ctx)
{
    if (ts_node_is_null(obj))
    {
        return;
    }
    std::string name = GetIdentifierName(obj, ctx.sourceCode);
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
        TSNode typeNode = parser::GetChildByField(obj, gs.fieldType);
        const auto typeInfo = ExtractTypeInfoFromAST(typeNode, ctx.sourceCode);
        if (typeInfo.isHandle)
        {
            std::string typeText = NodeText(typeNode, ctx.sourceCode);
            ctx.diagCtx.EmitAtRange(ToSourceRange(obj), diagnostics::codes::PossibleNullDereference, typeText,
                                    DiagnosticSeverity::Warning);
        }
    }
}

enum class ExprActionKind : uint8_t
{
    Eval,
    AfterLeftAnd,
    AfterLeftOr,
    RestoreVars
};

struct ExprStackFrame
{
    ExprActionKind kind;
    TSNode node;
    decltype(FlowState::vars) savedVars;
};

void DispatchNonBinaryExpr(TSNode expr, TSSymbol sym, const parser::GrammarSymbols& gs,
                           std::vector<ExprStackFrame>& stack, FlowState& state, NullCheckContext& ctx)
{
    if (sym == gs.symMemberExpression)
    {
        TSNode obj = parser::GetChildByField(expr, gs.fieldObject);
        CheckMemberDereference(obj, state, ctx);
        stack.push_back({ExprActionKind::Eval, obj, {}});
    }
    else if (sym == gs.symIndexExpression)
    {
        TSNode obj = parser::GetChildByField(expr, gs.fieldObject);
        CheckMemberDereference(obj, state, ctx);
        stack.push_back({ExprActionKind::Eval, parser::GetChildByField(expr, gs.fieldIndex), {}});
        stack.push_back({ExprActionKind::Eval, obj, {}});
    }
    else if (sym == gs.symCallExpression)
    {
        TSNode argsNode = parser::GetChildByField(expr, gs.fieldArguments);
        const uint32_t count = ts_node_is_null(argsNode) ? 0 : ts_node_named_child_count(argsNode);
        for (uint32_t i = count; i > 0; --i)
        {
            stack.push_back({ExprActionKind::Eval, ts_node_named_child(argsNode, i - 1), {}});
        }
        stack.push_back({ExprActionKind::Eval, parser::GetChildByField(expr, gs.fieldFunction), {}});
    }
    else if (sym == gs.symCastExpression)
    {
        stack.push_back({ExprActionKind::Eval, parser::GetChildByField(expr, gs.fieldValue), {}});
    }
    else if (sym == gs.symParenthesizedExpression)
    {
        stack.push_back({ExprActionKind::Eval, ts_node_named_child(expr, 0), {}});
    }
    else if (sym == gs.symTernaryExpression)
    {
        stack.push_back({ExprActionKind::Eval, parser::GetChildByField(expr, gs.fieldAlternative), {}});
        stack.push_back({ExprActionKind::Eval, parser::GetChildByField(expr, gs.fieldConsequence), {}});
        stack.push_back({ExprActionKind::Eval, parser::GetChildByField(expr, gs.fieldCondition), {}});
    }
}

void DispatchBinaryExpr(TSNode expr, const parser::GrammarSymbols& gs, std::vector<ExprStackFrame>& stack)
{
    TSNode opNode = parser::GetChildByField(expr, gs.fieldOperator);
    const TSSymbol opSym = ts_node_symbol(opNode);
    TSNode left = parser::GetChildByField(expr, gs.fieldLeft);
    TSNode right = parser::GetChildByField(expr, gs.fieldRight);

    if (opSym == gs.tokAmpAmp || opSym == gs.tokAnd)
    {
        stack.push_back({ExprActionKind::AfterLeftAnd, expr, {}});
        stack.push_back({ExprActionKind::Eval, left, {}});
    }
    else if (opSym == gs.tokPipePipe || opSym == gs.tokOr)
    {
        stack.push_back({ExprActionKind::AfterLeftOr, expr, {}});
        stack.push_back({ExprActionKind::Eval, left, {}});
    }
    else
    {
        stack.push_back({ExprActionKind::Eval, right, {}});
        stack.push_back({ExprActionKind::Eval, left, {}});
    }
}

void ProcessAfterLeft(const ExprStackFrame& frame, bool isAnd, const parser::GrammarSymbols& gs,
                      std::vector<ExprStackFrame>& stack, FlowState& state, NullCheckContext& ctx)
{
    TSNode left = parser::GetChildByField(frame.node, gs.fieldLeft);
    TSNode right = parser::GetChildByField(frame.node, gs.fieldRight);

    std::vector<NullAssertion> posAssertions;
    std::vector<NullAssertion> negAssertions;
    ExtractConditionAssertions(left, ctx.sourceCode, posAssertions, negAssertions);

    auto savedVars = state.vars;
    ApplyAssertions(state, isAnd ? posAssertions : negAssertions);

    stack.push_back({ExprActionKind::RestoreVars, TSNode{}, std::move(savedVars)});
    stack.push_back({ExprActionKind::Eval, right, {}});
}
} // namespace

void CheckNullExpression(TSNode expr, FlowState& state, NullCheckContext& ctx)
{
    if (ts_node_is_null(expr))
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    std::vector<ExprStackFrame> stack;
    stack.push_back({ExprActionKind::Eval, expr, {}});

    while (!stack.empty())
    {
        ExprStackFrame frame = std::move(stack.back());
        stack.pop_back();

        if (frame.kind == ExprActionKind::RestoreVars)
        {
            state.vars = std::move(frame.savedVars);
            continue;
        }
        if (frame.kind == ExprActionKind::AfterLeftAnd)
        {
            ProcessAfterLeft(frame, true, gs, stack, state, ctx);
            continue;
        }
        if (frame.kind == ExprActionKind::AfterLeftOr)
        {
            ProcessAfterLeft(frame, false, gs, stack, state, ctx);
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
            DispatchBinaryExpr(node, gs, stack);
        }
        else
        {
            DispatchNonBinaryExpr(node, sym, gs, stack, state, ctx);
        }
    }
}

} // namespace angel_lsp::analysis
