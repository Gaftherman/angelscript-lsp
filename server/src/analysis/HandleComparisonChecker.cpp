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

bool IsSupportedComparisonOp(std::string_view op)
{
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

bool IsHandleTypeNode(TSNode node, const std::string& typeName, const Scope* scope, const DiagnosticContext& ctx)
{
    if (typeName.ends_with('@') || typeName.find('@') != std::string::npos || typeName == "ref")
    {
        return true;
    }
    if (scope)
    {
        const std::string text = GetNodeText(node, ctx.request.sourceCode);
        if (const auto* def = ResolveInScope(scope, LastScopeSegment(text)))
        {
            if (def->isHandleType || def->typeName.ends_with('@'))
            {
                return true;
            }
        }
    }
    return !typeName.empty() && FindFuncdefSymbol(typeName, ctx.request.symbolTable).has_value();
}


bool IsNullNode(TSNode node, const std::string& typeName)
{
    if (IsNullInitializer(node) || typeName == "null")
    {
        return true;
    }
    const std::string_view nodeType = ts_node_type(node);
    return nodeType == "null_literal";
}

void CheckHandleEquality(TSNode opNode, std::string_view op, bool isHandleComparison, DiagnosticContext& ctx)
{
    if (!isHandleComparison)
    {
        return;
    }
    const int mode = ctx.request.diagnostics ? ctx.request.diagnostics->reportHandleComparisonEquality : 1;
    if (mode <= 0)
    {
        return;
    }
    const auto severity = (mode == 2) ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
    const std::string preferred = (op == "==") ? "is" : "!is";
    const TSPoint start = ts_node_start_point(opNode);
    const TSPoint end = ts_node_end_point(opNode);
    ctx.EmitAtRange({start.row, start.column, end.row, end.column},
                    diagnostics::codes::HandleComparisonEquality,
                    {std::string(op), preferred}, severity);
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

void DispatchComparisonCheck(TSNode opNode, std::string_view op, const ComparisonOperandsState& s,
                             DiagnosticContext& ctx)
{
    if (op == "==" || op == "!=")
    {
        const bool isHandleComp = (s.isLeftNull && s.isRightHandle) || (s.isRightNull && s.isLeftHandle);
        CheckHandleEquality(opNode, op, isHandleComp, ctx);
        return;
    }

    const RelationalCheckOperands ops{
        .hasNull = s.isLeftNull || s.isRightNull,
        .bothHandles = s.isLeftHandle && s.isRightHandle,
        .leftType = s.leftType,
    };
    CheckRelationalComparison(opNode, ops, ctx);
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
    if (!IsSupportedComparisonOp(op))
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
        .isLeftNull = IsNullNode(left, leftType),
        .isRightNull = IsNullNode(right, rightType),
        .isLeftHandle = IsHandleTypeNode(left, leftType, scope, ctx),
        .isRightHandle = IsHandleTypeNode(right, rightType, scope, ctx),
        .leftType = leftType,
    };
    DispatchComparisonCheck(opNode, op, operandsState, ctx);
}

} // namespace angel_lsp::analysis
