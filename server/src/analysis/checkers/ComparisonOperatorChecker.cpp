#include "analysis/checkers/ComparisonOperatorChecker.h"

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/OperandTypePair.h"

#include <string_view>

namespace angel_lsp::analysis
{

static bool ShouldSkipAddressComparison(const BinaryExpressionFact& fact, bool isRelational)
{
    return !isRelational && (fact.left.hasHandleOperator || fact.right.hasHandleOperator);
}

static bool AreCustomTypesCompatible(std::string_view op, std::string_view left, std::string_view right,
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

static bool CheckBoolCompatibility(bool isRelational, std::string_view other, const SymbolTable& table,
                                   std::string_view stringTypeName)
{
    if (IsNumericPrimitive(other) || other == stringTypeName || ResolvesToEnum(other, table))
    {
        return false;
    }
    if (TypeHasOperator(other, "opEquals", "bool", table) || TypeHasOpImplConvTo(other, "bool", table))
    {
        return !isRelational;
    }
    return false;
}

static bool CheckStringCompatibility(bool isRelational, std::string_view other, const SymbolTable& table,
                                     std::string_view stringTypeName)
{
    if (IsNumericPrimitive(other) || other == "bool" || ResolvesToEnum(other, table))
    {
        return false;
    }
    if (TypeHasOpImplConvTo(other, stringTypeName, table))
    {
        return true;
    }
    if (isRelational)
    {
        return TypeHasOperator(stringTypeName, "opCmp", other, table) ||
               TypeHasOperator(other, "opCmp", stringTypeName, table);
    }
    return TypeHasOperator(stringTypeName, "opEquals", other, table) ||
           TypeHasOperator(other, "opEquals", stringTypeName, table);
}

static bool IsEnumComparisonCompatible(std::string_view left, std::string_view right, const SymbolTable& table)
{
    const auto unwrappedLeft = UnwrapTypedef(left, table);
    const auto unwrappedRight = UnwrapTypedef(right, table);
    const bool leftEnum = ResolvesToEnum(unwrappedLeft, table);
    const bool rightEnum = ResolvesToEnum(unwrappedRight, table);
    if (leftEnum && rightEnum)
    {
        return true;
    }
    return (leftEnum && IsNumericPrimitive(unwrappedRight)) || (rightEnum && IsNumericPrimitive(unwrappedLeft));
}

static bool CheckIdenticalTypes(std::string_view op, std::string_view type, const SymbolTable& table,
                                std::string_view stringTypeName)
{
    if (type == stringTypeName || ResolvesToEnum(type, table))
    {
        return true;
    }
    return AreCustomTypesCompatible(op, type, type, table);
}

static bool CheckBoolBranch(bool isRelational, const OperandTypePair& types, const SymbolTable& table,
                            std::string_view stringTypeName)
{
    if (types.left == types.right)
    {
        return !isRelational;
    }
    const std::string_view other =
        (types.left == "bool") ? std::string_view(types.right) : std::string_view(types.left);
    return CheckBoolCompatibility(isRelational, other, table, stringTypeName);
}

[[nodiscard]] static bool IsStringOperand(std::string_view type, std::string_view stringTypeName) noexcept
{
    return type == stringTypeName;
}

static bool CheckStringBranch(bool isRelational, const OperandTypePair& types, const SymbolTable& table,
                              std::string_view stringTypeName)
{
    const bool leftStr = IsStringOperand(types.left, stringTypeName);
    const bool rightStr = IsStringOperand(types.right, stringTypeName);
    if (!leftStr && !rightStr)
    {
        return false;
    }
    const std::string_view other = leftStr ? std::string_view(types.right) : std::string_view(types.left);
    return CheckStringCompatibility(isRelational, other, table, stringTypeName);
}

static bool AreComparisonTypesCompatible(std::string_view op, const OperandTypePair& types, const SymbolTable& table,
                                         std::string_view stringTypeName)
{
    if (types.left == "void" || types.right == "void")
    {
        return false;
    }
    if (!IsKnownType(types.left, table, stringTypeName) || !IsKnownType(types.right, table, stringTypeName))
    {
        return true;
    }
    const auto unwrappedLeft = UnwrapTypedef(types.left, table);
    const auto unwrappedRight = UnwrapTypedef(types.right, table);
    if (IsNumericPrimitive(unwrappedLeft) && IsNumericPrimitive(unwrappedRight))
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

static void EmitComparisonDiagnostics(SourceRange range, std::string_view op, const OperandTypePair& types,
                                      DiagnosticContext& ctx)
{
    if (IsRelationalOp(op) && (types.left == "bool" || types.right == "bool"))
    {
        ctx.EmitAtRange(range, diagnostics::codes::IllegalOperation, DiagnosticSeverity::Error);
        return;
    }

    if (!AreComparisonTypesCompatible(op, types, ctx.request.symbolTable, ctx.request.GetEffectiveStringTypeName()))
    {
        ctx.EmitAtRange(range, diagnostics::codes::NoMatchingOperator, {op, types.left, types.right},
                        DiagnosticSeverity::Error);
    }
}

/**
 * @brief Check comparison operator compatibility.
 * @param[in] fact Binary expression facts with operator and both operands.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckComparisonOperatorCompatibility(const BinaryExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx)
{
    const bool isRelational = fact.opClass == BinaryOperatorClass::Relational || IsRelationalOp(fact.op);
    const bool isComparison = fact.opClass == BinaryOperatorClass::Equality || isRelational || IsComparisonOp(fact.op);
    if (!isComparison)
    {
        return;
    }

    if (ShouldSkipAddressComparison(fact, isRelational))
    {
        return;
    }

    const ExpressionTypeContext exprCtx(scope, ctx);
    if (const auto dt = IsBareDataType(fact.left.bytes, exprCtx))
    {
        ctx.EmitAtRange(fact.left.range, diagnostics::codes::ExpressionIsDataType, *dt);
        return;
    }
    if (const auto dt = IsBareDataType(fact.right.bytes, exprCtx))
    {
        ctx.EmitAtRange(fact.right.range, diagnostics::codes::ExpressionIsDataType, *dt);
        return;
    }
    if (IsFunctionReference(fact.left.bytes, exprCtx) || IsFunctionReference(fact.right.bytes, exprCtx))
    {
        ctx.EmitAtRange(fact.operatorRange, diagnostics::codes::NoMatchingOperator, {fact.op, "function", "value"});
        return;
    }

    const auto types = ResolveCleanBinaryOperandTypes(fact, scope, ctx);
    if (!types)
    {
        return;
    }

    EmitComparisonDiagnostics(fact.operatorRange, fact.op, *types, ctx);
}

} // namespace angel_lsp::analysis
