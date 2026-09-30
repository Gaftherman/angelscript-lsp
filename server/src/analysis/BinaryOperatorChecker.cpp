#include "analysis/BinaryOperatorChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/BinaryOperatorHelpers.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "parser/GrammarNames.h"
#include "parser/Primitives.h"

#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::analysis
{
namespace
{
[[nodiscard]] constexpr bool IsBitwiseOp(std::string_view op) noexcept
{
    return op == "|" || op == "&" || op == "^" || op == "<<" || op == ">>" || op == ">>>";
}

[[nodiscard]] constexpr bool IsArithmeticOp(std::string_view op) noexcept
{
    return op == "+" || op == "-" || op == "*" || op == "/" || op == "%" || op == "**";
}

std::pair<std::string_view, std::string_view> GetOpMethods(std::string_view op)
{
    if (op == "|") return {"opOr", "opOr_r"};
    if (op == "&") return {"opAnd", "opAnd_r"};
    if (op == "^") return {"opXor", "opXor_r"};
    if (op == "<<") return {"opShl", "opShl_r"};
    if (op == ">>") return {"opShr", "opShr_r"};
    if (op == ">>>") return {"opUShr", "opUShr_r"};
    if (op == "+") return {"opAdd", "opAdd_r"};
    if (op == "-") return {"opSub", "opSub_r"};
    if (op == "*") return {"opMul", "opMul_r"};
    if (op == "/") return {"opDiv", "opDiv_r"};
    if (op == "%") return {"opMod", "opMod_r"};
    if (op == "**") return {"opPow", "opPow_r"};
    return {"", ""};
}

bool IsIntegerOrEnum(const std::string& type, const SymbolTable& table)
{
    const std::string unwrapped = UnwrapTypedef(type, table);
    return parser::primitives::IsInteger(unwrapped) || ResolvesToEnum(unwrapped, table);
}

bool IsNumericOrEnum(const std::string& type, const SymbolTable& table)
{
    const std::string unwrapped = UnwrapTypedef(type, table);
    return parser::primitives::IsNumeric(unwrapped) || ResolvesToEnum(unwrapped, table);
}

bool AreBitwiseOperandsCompatible(std::string_view op, const BinaryOperandTypes& types, const SymbolTable& table)
{
    if (IsIntegerOrEnum(types.left, table) && IsIntegerOrEnum(types.right, table))
    {
        return true;
    }
    auto [opName, revOpName] = GetOpMethods(op);
    if (!opName.empty() && TypeHasOperator(types.left, std::string(opName), types.right, table))
    {
        return true;
    }
    if (!revOpName.empty() && TypeHasOperator(types.right, std::string(revOpName), types.left, table))
    {
        return true;
    }
    if (TypeHasOpImplConvTo(types.left, "int", table) && IsIntegerOrEnum(types.right, table))
    {
        return true;
    }
    if (IsIntegerOrEnum(types.left, table) && TypeHasOpImplConvTo(types.right, "int", table))
    {
        return true;
    }
    return false;
}

bool AreModuloOperandsCompatible(const BinaryOperandTypes& types, const SymbolTable& table)
{
    if (IsIntegerOrEnum(types.left, table) && IsIntegerOrEnum(types.right, table))
    {
        return true;
    }
    if (TypeHasOperator(types.left, "opMod", types.right, table) ||
        TypeHasOperator(types.right, "opMod_r", types.left, table))
    {
        return true;
    }
    return false;
}

bool IsStringConcatenationCompatible(const BinaryOperandTypes& types, const SymbolTable& table,
                                     std::string_view stringTypeName)
{
    const bool leftStr = (types.left == stringTypeName);
    const bool rightStr = (types.right == stringTypeName);
    if (!leftStr && !rightStr)
    {
        return false;
    }
    const std::string& other = leftStr ? types.right : types.left;
    return other == stringTypeName || parser::primitives::IsPrimitive(other) || ResolvesToEnum(other, table);
}

bool HasOverloadedArithmeticOperator(std::string_view op, const BinaryOperandTypes& types, const SymbolTable& table)
{
    auto [opName, revOpName] = GetOpMethods(op);
    if (!opName.empty() && TypeHasOperator(types.left, std::string(opName), types.right, table))
    {
        return true;
    }
    if (!revOpName.empty() && TypeHasOperator(types.right, std::string(revOpName), types.left, table))
    {
        return true;
    }
    return TypeHasOpImplConvTo(types.left, types.right, table) || TypeHasOpImplConvTo(types.right, types.left, table);
}

bool AreArithmeticOperandsCompatible(std::string_view op, const BinaryOperandTypes& types, const SymbolTable& table,
                                     std::string_view stringTypeName)
{
    if (op == "%")
    {
        return AreModuloOperandsCompatible(types, table);
    }
    if (IsNumericOrEnum(types.left, table) && IsNumericOrEnum(types.right, table))
    {
        return true;
    }
    if (op == "+" && IsStringConcatenationCompatible(types, table, stringTypeName))
    {
        return true;
    }
    return HasOverloadedArithmeticOperator(op, types, table);
}

bool AreBinaryOperandsCompatible(std::string_view op, const BinaryOperandTypes& types, const SymbolTable& table,
                                 std::string_view stringTypeName)
{
    if (!IsKnownType(types.left, table, stringTypeName) || !IsKnownType(types.right, table, stringTypeName))
    {
        return true;
    }
    if (types.left == "auto" || types.right == "auto" || types.left == "void" || types.right == "void" ||
        types.left.find('?') != std::string::npos || types.right.find('?') != std::string::npos)
    {
        return true;
    }
    if (IsBitwiseOp(op))
    {
        return AreBitwiseOperandsCompatible(op, types, table);
    }
    if (IsArithmeticOp(op))
    {
        return AreArithmeticOperandsCompatible(op, types, table, stringTypeName);
    }
    return true;
}
} // namespace

void CheckBinaryOperatorCompatibility(TSNode node, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode opNode = parser::GetChildByField(node, parser::fields::Operator);
    TSNode left = parser::GetChildByField(node, parser::fields::Left);
    TSNode right = parser::GetChildByField(node, parser::fields::Right);
    if (ts_node_is_null(opNode) || ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const std::string_view op = GetNodeTextView(opNode, ctx.request.sourceCode);
    if (!IsBitwiseOp(op) && !IsArithmeticOp(op))
    {
        return;
    }

    const auto types = ResolveCleanBinaryOperandTypes(left, right, scope, ctx);
    if (!types)
    {
        return;
    }

    if (!AreBinaryOperandsCompatible(op, *types, ctx.request.symbolTable, ctx.request.GetEffectiveStringTypeName()))
    {
        const TSPoint start = ts_node_start_point(opNode);
        const TSPoint end = ts_node_end_point(opNode);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::NoMatchingOperator,
                        {std::string(op), types->left, types->right}, DiagnosticSeverity::Error);
    }
}
} // namespace angel_lsp::analysis
