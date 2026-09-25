#include "analysis/NullSafetyExpr.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/NullSafetyCondition.h"
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
        if (it != state.vars.end() && (it->second == Nullability::Nullable || it->second == Nullability::DefinitelyNull))
        {
            if (!state.warnedVars.contains(name))
            {
                ctx.diagCtx.EmitAtRange(ToSourceRange(obj), diagnostics::codes::PossibleNullDereference, name,
                                        DiagnosticSeverity::Warning);
                state.warnedVars.insert(name);
            }
            it->second = Nullability::NonNull;
        }
        return;
    }
    std::string_view objType = ts_node_type(obj);
    if (objType == parser::nodes::CastExpression)
    {
        TSNode typeNode = parser::GetChildByField(obj, parser::fields::Type);
        std::string typeText = NodeText(typeNode, ctx.sourceCode);
        if (typeText.find('@') != std::string::npos)
        {
            ctx.diagCtx.EmitAtRange(ToSourceRange(obj), diagnostics::codes::PossibleNullDereference, typeText,
                                    DiagnosticSeverity::Warning);
        }
    }
}

void CheckBinaryExpr(TSNode expr, FlowState& state, NullCheckContext& ctx, int depth)
{
    TSNode left = parser::GetChildByField(expr, parser::fields::Left);
    TSNode opNode = parser::GetChildByField(expr, parser::fields::Operator);
    TSNode right = parser::GetChildByField(expr, parser::fields::Right);
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
    std::string_view exprType = ts_node_type(expr);
    if (exprType == parser::nodes::MemberExpression)
    {
        TSNode obj = parser::GetChildByField(expr, parser::fields::Object);
        CheckMemberDereference(obj, state, ctx);
        CheckNullExpression(obj, state, ctx, depth + 1);
    }
    else if (exprType == parser::nodes::IndexExpression)
    {
        TSNode obj = parser::GetChildByField(expr, parser::fields::Object);
        CheckMemberDereference(obj, state, ctx);
        CheckNullExpression(obj, state, ctx, depth + 1);
        CheckNullExpression(parser::GetChildByField(expr, parser::fields::Index), state, ctx, depth + 1);
    }
    else if (exprType == parser::nodes::CallExpression)
    {
        CheckNullExpression(parser::GetChildByField(expr, parser::fields::Function), state, ctx, depth + 1);
        TSNode argsNode = parser::GetChildByField(expr, parser::fields::Arguments);
        const uint32_t count = ts_node_is_null(argsNode) ? 0 : ts_node_named_child_count(argsNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            CheckNullExpression(ts_node_named_child(argsNode, i), state, ctx, depth + 1);
        }
    }
    else if (exprType == parser::nodes::BinaryExpression)
    {
        CheckBinaryExpr(expr, state, ctx, depth);
    }
    else if (exprType == parser::nodes::CastExpression)
    {
        CheckNullExpression(parser::GetChildByField(expr, parser::fields::Value), state, ctx, depth + 1);
    }
}

} // namespace angel_lsp::analysis
