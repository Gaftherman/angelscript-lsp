#include "analysis/checkers/ComparisonOperatorChecker.h"

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Primitives.h"

#include <optional>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

static bool ShouldSkipAddressComparison(const BinaryExpressionOperands& operands, bool isRelational,
                                        const NodeIndex* index)
{
    const auto handleOperator = parser::GetGrammarSymbols().tokAt;
    return !isRelational && (SyntaxOperands(operands.left, index).HasUnaryOperator(handleOperator) ||
                             SyntaxOperands(operands.right, index).HasUnaryOperator(handleOperator));
}

static bool AreCustomTypesCompatible(std::string_view op, const std::string& left, const std::string& right,
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

static bool CheckBoolCompatibility(bool isRelational, const std::string& other, const SymbolTable& table,
                                   std::string_view stringTypeName)
{
    if (parser::primitives::IsNumeric(other) || other == stringTypeName || ResolvesToEnum(other, table))
    {
        return false;
    }
    if (TypeHasOperator(other, "opEquals", "bool", table) || TypeHasOpImplConvTo(other, "bool", table))
    {
        return !isRelational;
    }
    return false;
}

static bool CheckStringCompatibility(bool isRelational, const std::string& other, const SymbolTable& table,
                                     std::string_view stringTypeName)
{
    if (parser::primitives::IsNumeric(other) || other == "bool" || ResolvesToEnum(other, table))
    {
        return false;
    }
    const std::string strTarget(stringTypeName);
    if (TypeHasOpImplConvTo(other, strTarget, table))
    {
        return true;
    }
    if (isRelational)
    {
        return TypeHasOperator(strTarget, "opCmp", other, table) || TypeHasOperator(other, "opCmp", strTarget, table);
    }
    return TypeHasOperator(strTarget, "opEquals", other, table) || TypeHasOperator(other, "opEquals", strTarget, table);
}

static bool IsEnumComparisonCompatible(const std::string& left, const std::string& right, const SymbolTable& table)
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

static bool CheckIdenticalTypes(std::string_view op, const std::string& type, const SymbolTable& table,
                                std::string_view stringTypeName)
{
    if (type == stringTypeName || ResolvesToEnum(type, table))
    {
        return true;
    }
    return AreCustomTypesCompatible(op, type, type, table);
}

static bool CheckBoolBranch(bool isRelational, const BinaryOperandTypes& types, const SymbolTable& table,
                            std::string_view stringTypeName)
{
    if (types.left == types.right)
    {
        return !isRelational;
    }
    return CheckBoolCompatibility(isRelational, (types.left == "bool") ? types.right : types.left, table,
                                  stringTypeName);
}

[[nodiscard]] static bool IsStringOperand(const std::string& type, std::string_view stringTypeName) noexcept
{
    return type == stringTypeName;
}

static bool CheckStringBranch(bool isRelational, const BinaryOperandTypes& types, const SymbolTable& table,
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

static bool AreComparisonTypesCompatible(std::string_view op, const BinaryOperandTypes& types, const SymbolTable& table,
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

static void EmitComparisonDiagnostics(TSNode opNode, std::string_view op, const BinaryOperandTypes& types,
                                      DiagnosticContext& ctx)
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

/**
 * @brief Check comparison operator compatibility.
 * @param[in] node AST node under evaluation.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckComparisonOperatorCompatibility(const BinaryExpressionOperands& operands, const Scope* scope,
                                          DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto& [opNode, left, right] = operands;

    const TSSymbol opSym = ts_node_symbol(opNode);
    if (!IsComparisonOpSym(opSym, gs))
    {
        return;
    }

    if (ShouldSkipAddressComparison(operands, IsRelationalOpSym(opSym, gs), ctx.request.nodeIndex))
    {
        return;
    }

    const std::string_view op = GetNodeTextView(opNode, ctx.request.sourceCode);

    const ExpressionTypeContext exprCtx(scope, ctx);
    if (const auto dt = IsBareDataType(left, exprCtx))
    {
        const TSPoint start = ts_node_start_point(left);
        const TSPoint end = ts_node_end_point(left);
        ctx.EmitAtRange(SourceRange{start.row, start.column, end.row, end.column},
                        diagnostics::codes::ExpressionIsDataType, *dt);
        return;
    }
    if (const auto dt = IsBareDataType(right, exprCtx))
    {
        const TSPoint start = ts_node_start_point(right);
        const TSPoint end = ts_node_end_point(right);
        ctx.EmitAtRange(SourceRange{start.row, start.column, end.row, end.column},
                        diagnostics::codes::ExpressionIsDataType, *dt);
        return;
    }
    if (IsFunctionReference(left, exprCtx) || IsFunctionReference(right, exprCtx))
    {
        const TSPoint start = ts_node_start_point(opNode);
        const TSPoint end = ts_node_end_point(opNode);
        ctx.EmitAtRange(SourceRange{start.row, start.column, end.row, end.column},
                        diagnostics::codes::NoMatchingOperator, {std::string_view(op), "function", "value"});
        return;
    }

    const auto types = ResolveCleanBinaryOperandTypes(left, right, scope, ctx);
    if (!types)
    {
        return;
    }

    EmitComparisonDiagnostics(opNode, op, *types, ctx);
}
} // namespace angel_lsp::analysis
