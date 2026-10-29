#include "analysis/HandleComparisonChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/BinaryOperatorHelpers.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
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

[[nodiscard]] inline ComparisonOperatorInfo ClassifyComparisonOperatorSym(TSSymbol sym,
                                                                          const parser::GrammarSymbols& gs) noexcept
{
    using namespace parser::keywords::operators;
    if (sym == gs.tokEqEq)
    {
        return {ComparisonCategory::ValueEquality, Is};
    }
    if (sym == gs.tokBangEq)
    {
        return {ComparisonCategory::ValueEquality, BangIs};
    }
    if (IsRelationalOpSym(sym, gs))
    {
        return {ComparisonCategory::Relational, {}};
    }
    return {};
}

void CheckHandleEquality(TSNode opNode, std::string_view op, std::string_view preferred, DiagnosticContext& ctx)
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
    ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::HandleComparisonEquality,
                    {std::string(op), std::string(preferred)}, severity);
}

void CheckRelationalComparison(TSNode opNode, const RelationalCheckOperands& ops, DiagnosticContext& ctx)
{
    const TSPoint start = ts_node_start_point(opNode);
    const TSPoint end = ts_node_end_point(opNode);

    if (ops.hasNull)
    {
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::IllegalOperation,
                        DiagnosticSeverity::Error);
        return;
    }

    if (ops.bothHandles)
    {
        const std::string_view arrayTypeName =
            (ctx.request.typeConfig && !ctx.request.typeConfig->arrayTypeName.empty())
                ? std::string_view(ctx.request.typeConfig->arrayTypeName)
                : ctx.request.symbolTable.GetDefaultArrayType();
        const std::string baseClass = MemberOwnerType(ops.leftType, arrayTypeName);
        if (!baseClass.empty() && ctx.request.symbolTable.FindSymbolsPtr(baseClass))
        {
            const auto opSyms = ctx.request.symbolTable.FindMemberSymbolPtr(baseClass, "opCmp");
            if (!opSyms || opSyms->empty())
            {
                ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::IllegalOperation,
                                DiagnosticSeverity::Error);
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
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
    if (ts_node_is_null(opNode))
    {
        return;
    }
    const TSSymbol opSym = ts_node_symbol(opNode);
    const ComparisonOperatorInfo opInfo = ClassifyComparisonOperatorSym(opSym, gs);
    if (opInfo.category == ComparisonCategory::None)
    {
        return;
    }

    TSNode left = parser::GetChildByField(node, gs.fieldLeft);
    TSNode right = parser::GetChildByField(node, gs.fieldRight);
    if (ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const std::string leftType = ResolveExpressionType(left, ExpressionTypeContext(scope, ctx));
    const std::string rightType = ResolveExpressionType(right, ExpressionTypeContext(scope, ctx));

    const ComparisonOperandsState operandsState{
        .isLeftNull = IsNullOperand(left, leftType),
        .isRightNull = IsNullOperand(right, rightType),
        .isLeftHandle = IsHandleType(leftType, ctx.request.symbolTable) || ts_node_symbol(left) == gs.symThisExpression,
        .isRightHandle =
            IsHandleType(rightType, ctx.request.symbolTable) || ts_node_symbol(right) == gs.symThisExpression,
        .leftType = leftType,
    };
    const ComparisonDispatchContext dispatch{
        .opNode = opNode,
        .op = GetNodeTextView(opNode, ctx.request.sourceCode),
        .opInfo = opInfo,
    };
    DispatchComparisonCheck(dispatch, operandsState, ctx);
}

} // namespace angel_lsp::analysis
