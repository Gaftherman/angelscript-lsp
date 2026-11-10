#include "analysis/checkers/BinaryOperatorChecker.h"

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/OperandTypePair.h"

#include <string_view>
#include <utility>

namespace angel_lsp::analysis
{
/**
 * @brief Maps binary operator spelling to its canonical AngelScript script method and right-hand mirror method names.
 * @param[in] op Operator symbol spelling.
 * @return Pair containing the left-hand method name and right-hand fallback method name.
 */
static std::pair<std::string_view, std::string_view> GetOpMethods(std::string_view op)
{
    if (op == "|")
        return {"opOr", "opOr_r"};
    if (op == "&")
        return {"opAnd", "opAnd_r"};
    if (op == "^")
        return {"opXor", "opXor_r"};
    if (op == "<<")
        return {"opShl", "opShl_r"};
    if (op == ">>")
        return {"opShr", "opShr_r"};
    if (op == ">>>")
        return {"opUShr", "opUShr_r"};
    if (op == "+")
        return {"opAdd", "opAdd_r"};
    if (op == "-")
        return {"opSub", "opSub_r"};
    if (op == "*")
        return {"opMul", "opMul_r"};
    if (op == "/")
        return {"opDiv", "opDiv_r"};
    if (op == "%")
        return {"opMod", "opMod_r"};
    if (op == "**")
        return {"opPow", "opPow_r"};
    return {"", ""};
}

/**
 * @brief Tests whether a type is an integer primitive or resolves to an enum.
 * @param[in] type Resolved operand type spelling.
 * @param[in] table Symbol table used to resolve enum names.
 * @return True for integer primitives and enum types.
 */
static bool IsIntegerOrEnum(std::string_view type, const SymbolTable& table)
{
    const auto unwrapped = UnwrapTypedef(type, table);
    return IsIntegerPrimitive(unwrapped) || ResolvesToEnum(unwrapped, table);
}

/**
 * @brief Tests whether a type is a numeric primitive or resolves to an enum.
 * @param[in] type Resolved operand type spelling.
 * @param[in] table Symbol table used to resolve enum names.
 * @return True for numeric primitives and enum types.
 */
static bool IsNumericOrEnum(std::string_view type, const SymbolTable& table)
{
    const auto unwrapped = UnwrapTypedef(type, table);
    return IsNumericPrimitive(unwrapped) || ResolvesToEnum(unwrapped, table);
}

static bool AreBitwiseOperandsCompatible(std::string_view op, const OperandTypePair& types, const SymbolTable& table)
{
    if (IsIntegerOrEnum(types.left, table) && IsIntegerOrEnum(types.right, table))
    {
        return true;
    }
    auto [opName, revOpName] = GetOpMethods(op);
    if (!opName.empty() && TypeHasOperator(types.left, opName, types.right, table))
    {
        return true;
    }
    if (!revOpName.empty() && TypeHasOperator(types.right, revOpName, types.left, table))
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

static bool AreModuloOperandsCompatible(const OperandTypePair& types, const SymbolTable& table)
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

static bool IsStringConcatenationCompatible(const OperandTypePair& types, const SymbolTable& table,
                                            std::string_view stringTypeName)
{
    const bool leftStr = (types.left == stringTypeName);
    const bool rightStr = (types.right == stringTypeName);
    if (!leftStr && !rightStr)
    {
        return false;
    }
    const std::string_view other = leftStr ? std::string_view(types.right) : std::string_view(types.left);
    return other == stringTypeName || IsPrimitiveTypeName(other) || ResolvesToEnum(other, table);
}

static bool HasClassNumericOpImplConv(std::string_view classType, std::string_view otherType, const SymbolTable& table)
{
    if (TypeHasOpImplConvTo(classType, otherType, table))
    {
        return true;
    }
    if (!IsNumericPrimitive(otherType))
    {
        return false;
    }
    return TypeHasOpImplConvTo(classType, "int", table) || TypeHasOpImplConvTo(classType, "double", table) ||
           TypeHasOpImplConvTo(classType, "float", table);
}

static bool HasOverloadedArithmeticOperator(std::string_view op, const OperandTypePair& types, const SymbolTable& table)
{
    auto [opName, revOpName] = GetOpMethods(op);
    if (!opName.empty() && TypeHasOperator(types.left, opName, types.right, table))
    {
        return true;
    }
    if (!revOpName.empty() && TypeHasOperator(types.right, revOpName, types.left, table))
    {
        return true;
    }
    const bool leftIsClass = !IsPrimitiveTypeName(types.left) && !ResolvesToEnum(types.left, table);
    const bool rightIsClass = !IsPrimitiveTypeName(types.right) && !ResolvesToEnum(types.right, table);
    if (leftIsClass && !rightIsClass)
    {
        return HasClassNumericOpImplConv(types.left, types.right, table);
    }
    if (!leftIsClass && rightIsClass)
    {
        return HasClassNumericOpImplConv(types.right, types.left, table);
    }
    return TypeHasOpImplConvTo(types.left, types.right, table) || TypeHasOpImplConvTo(types.right, types.left, table);
}

static bool AreArithmeticOperandsCompatible(std::string_view op, const OperandTypePair& types, const SymbolTable& table,
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

static bool AreBinaryOperandsCompatible(std::string_view op, const OperandTypePair& types, const SymbolTable& table,
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
    if (types.left == "auto" || types.right == "auto" ||
        std::string_view(types.left).find('?') != std::string_view::npos ||
        std::string_view(types.right).find('?') != std::string_view::npos)
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

/**
 * @brief Check binary operator compatibility.
 * @param[in] fact Binary expression facts with operator and both operands.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckBinaryOperatorCompatibility(const BinaryExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx)
{
    const bool isBitwise = fact.opClass == BinaryOperatorClass::Bitwise || IsBitwiseOp(fact.op);
    const bool isArithmetic = fact.opClass == BinaryOperatorClass::Arithmetic || IsArithmeticOp(fact.op);
    if (!isBitwise && !isArithmetic)
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

    if (!AreBinaryOperandsCompatible(fact.op, *types, ctx.request.symbolTable,
                                     ctx.request.GetEffectiveStringTypeName()))
    {
        ctx.EmitAtRange(fact.operatorRange, diagnostics::codes::NoMatchingOperator,
                        {fact.op, types->left, types->right});
    }
}
} // namespace angel_lsp::analysis
