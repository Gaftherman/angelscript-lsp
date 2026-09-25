#include "analysis/HandleComparisonChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "parser/GrammarNames.h"
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{
namespace
{
enum class ComparisonCategory
{
    None,
    ValueEquality,
    Relational
};

struct ComparisonOperatorInfo
{
    ComparisonCategory category = ComparisonCategory::None;
    std::string_view identityEquivalent;
};

struct ComparisonDispatchContext
{
    TSNode opNode;
    std::string_view op;
    ComparisonOperatorInfo opInfo;
};

struct RelationalCheckOperands
{
    bool hasNull = false;
    bool bothHandles = false;
    std::string leftType;
};

struct ComparisonOperandsState
{
    bool isLeftNull = false;
    bool isRightNull = false;
    bool isLeftHandle = false;
    bool isRightHandle = false;
    std::string leftType;
};

[[nodiscard]] constexpr ComparisonOperatorInfo ClassifyComparisonOperator(std::string_view op) noexcept
{
    if (op == "==")
    {
        return {ComparisonCategory::ValueEquality, "is"};
    }
    if (op == "!=")
    {
        return {ComparisonCategory::ValueEquality, "!is"};
    }
    if (op == "<" || op == "<=" || op == ">" || op == ">=")
    {
        return {ComparisonCategory::Relational, {}};
    }
    return {};
}

[[nodiscard]] bool IsNullOperand(TSNode node, std::string_view typeName) noexcept
{
    if (typeName == "null" || IsNullInitializer(node))
    {
        return true;
    }
    return !ts_node_is_null(node) &&
           std::string_view(ts_node_type(node)) == parser::nodes::NullLiteral;
}

void CheckHandleEquality(TSNode opNode, std::string_view op, std::string_view preferred,
                         DiagnosticContext& ctx)
{
    if (preferred.empty())
    {
        return;
    }
    const int mode = ctx.request.diagnostics ? ctx.request.diagnostics->reportHandleComparisonEquality : 1;
    if (mode <= 0)
    {
        return;
    }
    const auto severity = (mode == 2) ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
    const TSPoint start = ts_node_start_point(opNode);
    const TSPoint end = ts_node_end_point(opNode);
    ctx.EmitAtRange({start.row, start.column, end.row, end.column},
                    diagnostics::codes::HandleComparisonEquality,
                    {std::string(op), std::string(preferred)}, severity);
}

void CheckRelationalComparison(TSNode opNode, const RelationalCheckOperands& ops, DiagnosticContext& ctx)
{
    const TSPoint start = ts_node_start_point(opNode);
    const TSPoint end = ts_node_end_point(opNode);

    if (ops.hasNull)
    {
        ctx.EmitAtRange({start.row, start.column, end.row, end.column},
                        diagnostics::codes::IllegalOperation, DiagnosticSeverity::Error);
        return;
    }

    if (ops.bothHandles)
    {
        const std::string baseClass = MemberOwnerType(ops.leftType);
        if (!baseClass.empty() && ctx.request.symbolTable.FindSymbolsPtr(baseClass))
        {
            const auto opSyms = ctx.request.symbolTable.FindSymbolsPtr(baseClass + "::opCmp");
            if (!opSyms || opSyms->empty())
            {
                ctx.EmitAtRange({start.row, start.column, end.row, end.column},
                                diagnostics::codes::IllegalOperation, DiagnosticSeverity::Error);
            }
        }
    }
}

void DispatchComparisonCheck(const ComparisonDispatchContext& dispatch, const ComparisonOperandsState& s,
                             DiagnosticContext& ctx)
{
    if (dispatch.opInfo.category == ComparisonCategory::ValueEquality)
    {
        const bool isHandleComp = (s.isLeftNull && s.isRightHandle) || (s.isRightNull && s.isLeftHandle);
        if (isHandleComp)
        {
            CheckHandleEquality(dispatch.opNode, dispatch.op, dispatch.opInfo.identityEquivalent, ctx);
        }
        return;
    }

    const RelationalCheckOperands ops{
        .hasNull = s.isLeftNull || s.isRightNull,
        .bothHandles = s.isLeftHandle && s.isRightHandle,
        .leftType = s.leftType,
    };
    CheckRelationalComparison(dispatch.opNode, ops, ctx);
}
} // namespace

void CheckHandleComparison(TSNode node, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode opNode = parser::GetChildByField(node, parser::fields::Operator);
    if (ts_node_is_null(opNode))
    {
        return;
    }
    const std::string op = GetNodeText(opNode, ctx.request.sourceCode);
    const ComparisonOperatorInfo opInfo = ClassifyComparisonOperator(op);
    if (opInfo.category == ComparisonCategory::None)
    {
        return;
    }

    TSNode left = parser::GetChildByField(node, parser::fields::Left);
    TSNode right = parser::GetChildByField(node, parser::fields::Right);
    if (ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const std::string leftType = ResolveExpressionType(
        left, {scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.fileUri});
    const std::string rightType = ResolveExpressionType(
        right, {scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.fileUri});

    const ComparisonOperandsState operandsState{
        .isLeftNull = IsNullOperand(left, leftType),
        .isRightNull = IsNullOperand(right, rightType),
        .isLeftHandle = IsHandleType(leftType, ctx.request.symbolTable),
        .isRightHandle = IsHandleType(rightType, ctx.request.symbolTable),
        .leftType = leftType,
    };
    const ComparisonDispatchContext dispatch{
        .opNode = opNode,
        .op = op,
        .opInfo = opInfo,
    };
    DispatchComparisonCheck(dispatch, operandsState, ctx);
}

} // namespace angel_lsp::analysis
