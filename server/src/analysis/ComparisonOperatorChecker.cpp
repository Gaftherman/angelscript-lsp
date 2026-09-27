#include "analysis/ComparisonOperatorChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "parser/GrammarNames.h"
#include "parser/Primitives.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
struct OperandTypes
{
    std::string left;
    std::string right;
};

[[nodiscard]] constexpr bool IsComparisonOp(std::string_view op) noexcept
{
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

[[nodiscard]] constexpr bool IsRelationalOp(std::string_view op) noexcept
{
    return op == "<" || op == "<=" || op == ">" || op == ">=";
}

bool IsHandleAddressOperand(TSNode node)
{
    if (ts_node_is_null(node) || std::string_view(ts_node_type(node)) != "unary_expression")
    {
        return false;
    }
    TSNode op = parser::GetChildByField(node, parser::fields::Operator);
    return !ts_node_is_null(op) && std::string_view(ts_node_type(op)) == "@";
}

bool ShouldSkipAddressComparison(TSNode left, TSNode right, bool isRelational)
{
    return !isRelational && (IsHandleAddressOperand(left) || IsHandleAddressOperand(right));
}

bool IsParameterCompatible(const ParameterInformation& param, const std::string& argType, const SymbolTable& table)
{
    const std::string cleanParam = CleanBaseType(param.typeName);
    if (cleanParam == "?" || cleanParam == "?&" || param.typeName.find('?') != std::string::npos)
    {
        return true;
    }
    if (cleanParam == argType || param.typeName == argType)
    {
        return true;
    }
    const auto hierarchy = GetInheritedTypeHierarchy(argType, table);
    return std::find(hierarchy.begin(), hierarchy.end(), cleanParam) != hierarchy.end();
}

bool TypeHasOperator(const std::string& typeName, const std::string& opName, const std::string& argType,
                     const SymbolTable& table)
{
    for (const auto& cls : GetInheritedTypeHierarchy(typeName, table))
    {
        const auto symbols = table.FindMemberSymbolPtr(cls, opName);
        if (!symbols)
        {
            continue;
        }
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Function && !sym.GetFunction().parameters.empty() &&
                IsParameterCompatible(sym.GetFunction().parameters.front(), argType, table))
            {
                return true;
            }
        }
    }
    return false;
}

bool IsKnownType(const std::string& type, const SymbolTable& table, std::string_view stringTypeName = "string")
{
    if (parser::primitives::IsNumeric(type) || type == "bool" || type == stringTypeName || type == "string" ||
        ResolvesToEnum(type, table))
    {
        return true;
    }
    const auto symbols = table.FindSymbolsPtr(type);
    return symbols && !symbols->empty();
}

bool AreCustomTypesCompatible(std::string_view op, const std::string& left, const std::string& right,
                              const SymbolTable& table)
{
    if (TypeHasOpImplConvTo(left, right, table) || TypeHasOpImplConvTo(right, left, table))
    {
        return true;
    }
    if (IsRelationalOp(op))
    {
        return TypeHasOperator(left, "opCmp", right, table) || TypeHasOperator(right, "opCmp", left, table);
    }
    return TypeHasOperator(left, "opEquals", right, table) || TypeHasOperator(right, "opEquals", left, table) ||
           TypeHasOperator(left, "opCmp", right, table) || TypeHasOperator(right, "opCmp", left, table);
}

bool CheckBoolCompatibility(bool isRelational, const std::string& other, const SymbolTable& table,
                            std::string_view stringTypeName)
{
    if (parser::primitives::IsNumeric(other) || other == stringTypeName || other == "string" ||
        ResolvesToEnum(other, table))
    {
        return false;
    }
    if (TypeHasOperator(other, "opEquals", "bool", table) || TypeHasOpImplConvTo(other, "bool", table))
    {
        return !isRelational;
    }
    return false;
}

bool CheckStringCompatibility(bool isRelational, const std::string& other, const SymbolTable& table,
                              std::string_view stringTypeName)
{
    if (parser::primitives::IsNumeric(other) || other == "bool" || ResolvesToEnum(other, table))
    {
        return false;
    }
    const std::string strTarget(stringTypeName);
    if (TypeHasOpImplConvTo(other, strTarget, table) ||
        (strTarget != "string" && TypeHasOpImplConvTo(other, "string", table)))
    {
        return true;
    }
    if (isRelational)
    {
        return TypeHasOperator(strTarget, "opCmp", other, table) || TypeHasOperator(other, "opCmp", strTarget, table);
    }
    return TypeHasOperator(strTarget, "opEquals", other, table) ||
           TypeHasOperator(other, "opEquals", strTarget, table);
}

bool IsEnumComparisonCompatible(const std::string& left, const std::string& right, const SymbolTable& table)
{
    const std::string unwrappedLeft = UnwrapTypedef(left, table);
    const std::string unwrappedRight = UnwrapTypedef(right, table);
    const bool leftEnum = ResolvesToEnum(unwrappedLeft, table);
    const bool rightEnum = ResolvesToEnum(unwrappedRight, table);
    if (leftEnum && rightEnum)
    {
        return true;
    }
    return (leftEnum && parser::primitives::IsNumeric(unwrappedRight)) ||
           (rightEnum && parser::primitives::IsNumeric(unwrappedLeft));
}

bool CheckIdenticalTypes(std::string_view op, const std::string& type, const SymbolTable& table,
                         std::string_view stringTypeName)
{
    if (type == stringTypeName || type == "string" || ResolvesToEnum(type, table))
    {
        return true;
    }
    return AreCustomTypesCompatible(op, type, type, table);
}

bool CheckBoolBranch(bool isRelational, const OperandTypes& types, const SymbolTable& table,
                     std::string_view stringTypeName)
{
    if (types.left == types.right)
    {
        return !isRelational;
    }
    return CheckBoolCompatibility(isRelational, (types.left == "bool") ? types.right : types.left, table,
                                  stringTypeName);
}

[[nodiscard]] bool IsStringOperand(const std::string& type, std::string_view stringTypeName) noexcept
{
    return type == stringTypeName || type == "string";
}

bool CheckStringBranch(bool isRelational, const OperandTypes& types, const SymbolTable& table,
                       std::string_view stringTypeName)
{
    const bool leftStr = IsStringOperand(types.left, stringTypeName);
    const bool rightStr = IsStringOperand(types.right, stringTypeName);
    if (!leftStr && !rightStr)
    {
        return false;
    }
    const std::string& other = leftStr ? types.right : types.left;
    return CheckStringCompatibility(isRelational, other, table, stringTypeName);
}

bool AreComparisonTypesCompatible(std::string_view op, const OperandTypes& types, const SymbolTable& table,
                                  std::string_view stringTypeName = "string")
{
    if (!IsKnownType(types.left, table, stringTypeName) || !IsKnownType(types.right, table, stringTypeName))
    {
        return true;
    }
    const std::string unwrappedLeft = UnwrapTypedef(types.left, table);
    const std::string unwrappedRight = UnwrapTypedef(types.right, table);
    if (parser::primitives::IsNumeric(unwrappedLeft) && parser::primitives::IsNumeric(unwrappedRight))
    {
        return true;
    }
    const bool isRelational = IsRelationalOp(op);
    if (types.left == "bool" || types.right == "bool")
    {
        return CheckBoolBranch(isRelational, types, table, stringTypeName);
    }
    if (types.left == types.right || unwrappedLeft == unwrappedRight)
    {
        return CheckIdenticalTypes(op, unwrappedLeft, table, stringTypeName);
    }
    if (IsEnumComparisonCompatible(types.left, types.right, table))
    {
        return true;
    }
    if (IsStringOperand(types.left, stringTypeName) || IsStringOperand(types.right, stringTypeName))
    {
        return CheckStringBranch(isRelational, types, table, stringTypeName);
    }
    return AreCustomTypesCompatible(op, types.left, types.right, table);
}

bool IsNullOperand(TSNode node, const std::string& type)
{
    return type.empty() || type == "null" || IsNullInitializer(node);
}

std::optional<OperandTypes> ResolveCleanOperandTypes(TSNode left, TSNode right, const Scope* scope,
                                                     const DiagnosticContext& ctx)
{
    const ExpressionTypeContext exprCtx{scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.fileUri,
                                        ctx.request.GetEffectiveStringTypeName(),
                                        ctx.request.GetEffectiveArrayTypeName()};
    const std::string rawLeft = ResolveExpressionType(left, exprCtx);
    const std::string rawRight = ResolveExpressionType(right, exprCtx);
    if (IsNullOperand(left, rawLeft) || IsNullOperand(right, rawRight))
    {
        return std::nullopt;
    }
    std::string cleanLeft = CleanBaseType(rawLeft, ctx.request.GetEffectiveArrayTypeName());
    std::string cleanRight = CleanBaseType(rawRight, ctx.request.GetEffectiveArrayTypeName());
    if (cleanLeft.empty() || cleanRight.empty())
    {
        return std::nullopt;
    }
    return OperandTypes{std::move(cleanLeft), std::move(cleanRight)};
}

void EmitComparisonDiagnostics(TSNode opNode, std::string_view op, const OperandTypes& types, DiagnosticContext& ctx)
{
    const TSPoint start = ts_node_start_point(opNode);
    const TSPoint end = ts_node_end_point(opNode);

    if (IsRelationalOp(op) && (types.left == "bool" || types.right == "bool"))
    {
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::IllegalOperation,
                        DiagnosticSeverity::Error);
        return;
    }

    if (!AreComparisonTypesCompatible(op, types, ctx.request.symbolTable, ctx.request.GetEffectiveStringTypeName()))
    {
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::NoMatchingOperator,
                        {std::string(op), types.left, types.right}, DiagnosticSeverity::Error);
    }
}
} // namespace

bool TypeHasOpImplConvTo(const std::string& typeName, const std::string& targetType, const SymbolTable& table)
{
    for (const auto& cls : GetInheritedTypeHierarchy(typeName, table))
    {
        const auto symbols = table.FindMemberSymbolPtr(cls, "opImplConv");
        if (!symbols)
        {
            continue;
        }
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Function && CleanBaseType(sym.GetFunction().returnType) == targetType)
            {
                return true;
            }
        }
    }
    return false;
}

void CheckComparisonOperatorCompatibility(TSNode node, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode opNode = parser::GetChildByField(node, parser::fields::Operator);
    TSNode left = parser::GetChildByField(node, parser::fields::Left);
    TSNode right = parser::GetChildByField(node, parser::fields::Right);
    if (ts_node_is_null(opNode) || ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const std::string_view op = GetNodeTextView(opNode, ctx.request.sourceCode);
    if (!IsComparisonOp(op))
    {
        return;
    }

    if (ShouldSkipAddressComparison(left, right, IsRelationalOp(op)))
    {
        return;
    }

    const auto types = ResolveCleanOperandTypes(left, right, scope, ctx);
    if (!types)
    {
        return;
    }

    EmitComparisonDiagnostics(opNode, op, *types, ctx);
}
} // namespace angel_lsp::analysis
