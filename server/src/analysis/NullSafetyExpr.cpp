#include "analysis/NullSafetyExpr.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/NullSafetyCondition.h"
#include "analysis/TypeExtraction.h"
#include "parser/GrammarNames.h"
#include <string>

namespace angel_lsp::analysis
{
namespace
{
SourceRange ToSourceRange(TSNode node)
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return SourceRange{start.row, start.column, end.row, end.column};
}

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

void CheckBinaryExpr(TSNode expr, FlowState& state, NullCheckContext& ctx, int depth)
{
    TSNode left = parser::GetChildByField(expr, parser::GetGrammarSymbols().fieldLeft);
    TSNode opNode = parser::GetChildByField(expr, parser::GetGrammarSymbols().fieldOperator);
    TSNode right = parser::GetChildByField(expr, parser::GetGrammarSymbols().fieldRight);
    std::string op = NodeText(opNode, ctx.sourceCode);

    if (op == "&&" || op == "and")
    {
        CheckNullExpression(left, state, ctx, depth + 1);
        std::vector<NullAssertion> posAssertions;
        std::vector<NullAssertion> negAssertions;
        ExtractConditionAssertions(left, ctx.sourceCode, posAssertions, negAssertions);
        FlowState tempState = state;
        ApplyAssertions(tempState, posAssertions);
        CheckNullExpression(right, tempState, ctx, depth + 1);
    }
    else if (op == "||" || op == "or")
    {
        CheckNullExpression(left, state, ctx, depth + 1);
        std::vector<NullAssertion> posAssertions;
        std::vector<NullAssertion> negAssertions;
        ExtractConditionAssertions(left, ctx.sourceCode, posAssertions, negAssertions);
        FlowState tempState = state;
        ApplyAssertions(tempState, negAssertions);
        CheckNullExpression(right, tempState, ctx, depth + 1);
    }
    else
    {
        CheckNullExpression(left, state, ctx, depth + 1);
        CheckNullExpression(right, state, ctx, depth + 1);
    }
}
} // namespace

void CheckNullExpression(TSNode expr, FlowState& state, NullCheckContext& ctx, int depth)
{
    if (ts_node_is_null(expr) || depth >= 64)
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol exprSym = ts_node_symbol(expr);
    if (exprSym == gs.symMemberExpression)
    {
        TSNode obj = parser::GetChildByField(expr, gs.fieldObject);
        CheckMemberDereference(obj, state, ctx);
        CheckNullExpression(obj, state, ctx, depth + 1);
    }
    else if (exprSym == gs.symIndexExpression)
    {
        TSNode obj = parser::GetChildByField(expr, gs.fieldObject);
        CheckMemberDereference(obj, state, ctx);
        CheckNullExpression(obj, state, ctx, depth + 1);
        CheckNullExpression(parser::GetChildByField(expr, gs.fieldIndex), state, ctx, depth + 1);
    }
    else if (exprSym == gs.symCallExpression)
    {
        CheckNullExpression(parser::GetChildByField(expr, parser::GetGrammarSymbols().fieldFunction), state, ctx,
                            depth + 1);
        TSNode argsNode = parser::GetChildByField(expr, parser::GetGrammarSymbols().fieldArguments);
        const uint32_t count = ts_node_is_null(argsNode) ? 0 : ts_node_named_child_count(argsNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            CheckNullExpression(ts_node_named_child(argsNode, i), state, ctx, depth + 1);
        }
    }
    else if (exprSym == gs.symBinaryExpression)
    {
        CheckBinaryExpr(expr, state, ctx, depth);
    }
    else if (exprSym == gs.symCastExpression)
    {
        CheckNullExpression(parser::GetChildByField(expr, gs.fieldValue), state, ctx, depth + 1);
    }
    else if (exprSym == gs.symParenthesizedExpression)
    {
        CheckNullExpression(ts_node_named_child(expr, 0), state, ctx, depth + 1);
    }
    else if (exprSym == gs.symTernaryExpression)
    {
        CheckNullExpression(parser::GetChildByField(expr, gs.fieldCondition), state, ctx, depth + 1);
        CheckNullExpression(parser::GetChildByField(expr, gs.fieldConsequence), state, ctx, depth + 1);
        CheckNullExpression(parser::GetChildByField(expr, gs.fieldAlternative), state, ctx, depth + 1);
    }
}

} // namespace angel_lsp::analysis
