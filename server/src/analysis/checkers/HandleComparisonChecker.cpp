#include "analysis/checkers/HandleComparisonChecker.h"

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/TypeSanitization.h"

#include <string_view>

namespace angel_lsp::analysis
{

[[nodiscard]] static ComparisonOperatorInfo ClassifyComparisonOperator(const BinaryExpressionFact& fact) noexcept
{
    if (fact.opClass == BinaryOperatorClass::Equality)
    {
        if (fact.op == "==")
        {
            return {ComparisonCategory::ValueEquality, "is"};
        }
        if (fact.op == "!=")
        {
            return {ComparisonCategory::ValueEquality, "!is"};
        }
    }
    if (fact.opClass == BinaryOperatorClass::Relational)
    {
        return {ComparisonCategory::Relational, {}};
    }
    return {};
}

static void CheckHandleEquality(SourceRange opRange, std::string_view op, std::string_view preferred,
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
    ctx.EmitAtRange(opRange, diagnostics::codes::HandleComparisonEquality, {op, preferred}, severity);
}

static void CheckRelationalComparison(SourceRange opRange, const RelationalCheckOperands& ops, DiagnosticContext& ctx)
{
    if (ops.hasNull)
    {
        ctx.EmitAtRange(opRange, diagnostics::codes::IllegalOperation, DiagnosticSeverity::Error);
        return;
    }

    if (ops.bothHandles)
    {
        const std::string_view arrayTypeName =
            (ctx.request.typeConfig && !ctx.request.typeConfig->arrayTypeName.empty())
                ? std::string_view(ctx.request.typeConfig->arrayTypeName)
                : ctx.request.symbolTable.GetDefaultArrayType();
        const auto baseClass = MemberOwnerType(ops.leftType, arrayTypeName);
        if (!baseClass.empty() && ctx.request.symbolTable.FindSymbolsPtr(baseClass))
        {
            const auto opSyms = ctx.request.symbolTable.FindMemberSymbolPtr(baseClass, "opCmp");
            if (!opSyms || opSyms->empty())
            {
                ctx.EmitAtRange(opRange, diagnostics::codes::IllegalOperation, DiagnosticSeverity::Error);
            }
        }
    }
}

static void DispatchComparisonCheck(const ComparisonDispatchContext& dispatch, const ComparisonOperandsState& s,
                                    DiagnosticContext& ctx)
{
    if (dispatch.opInfo.category == ComparisonCategory::ValueEquality)
    {
        const bool isHandleComp = (s.isLeftNull && s.isRightHandle) || (s.isRightNull && s.isLeftHandle);
        if (isHandleComp)
        {
            CheckHandleEquality(dispatch.opRange, dispatch.op, dispatch.opInfo.identityEquivalent, ctx);
        }
        return;
    }

    const RelationalCheckOperands ops{
        .hasNull = s.isLeftNull || s.isRightNull,
        .bothHandles = s.isLeftHandle && s.isRightHandle,
        .leftType = s.leftType,
    };
    CheckRelationalComparison(dispatch.opRange, ops, ctx);
}

/**
 * @brief Validates handle equality and relational operations in binary expressions.
 * @param[in] fact Binary expression facts with operator and both operands.
 * @param[in] scope Lexical scope enclosing the expression.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckHandleComparison(const BinaryExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx)
{
    const ComparisonOperatorInfo opInfo = ClassifyComparisonOperator(fact);
    if (opInfo.category == ComparisonCategory::None)
    {
        return;
    }

    const auto leftType = ResolveExpressionType(fact.left.bytes, ExpressionTypeContext(scope, ctx));
    const auto rightType = ResolveExpressionType(fact.right.bytes, ExpressionTypeContext(scope, ctx));

    const auto& table = ctx.request.symbolTable;
    const ComparisonOperandsState operandsState{
        .isLeftNull = IsNullOperand(fact.left, leftType),
        .isRightNull = IsNullOperand(fact.right, rightType),
        .isLeftHandle = IsHandleType(leftType, table) || fact.left.isThis,
        .isRightHandle = IsHandleType(rightType, table) || fact.right.isThis,
        .leftType = leftType,
    };
    const ComparisonDispatchContext dispatch{
        .opRange = fact.operatorRange,
        .op = fact.op,
        .opInfo = opInfo,
    };
    DispatchComparisonCheck(dispatch, operandsState, ctx);
}

} // namespace angel_lsp::analysis
