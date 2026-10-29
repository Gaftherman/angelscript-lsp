#include "analysis/BinaryOperatorChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/BinaryOperatorHelpers.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
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
    using namespace parser::keywords::operators;
    return op == Pipe || op == Amp || op == Caret || op == LtLt || op == GtGt || op == GtGtGt;
}

[[nodiscard]] constexpr bool IsArithmeticOp(std::string_view op) noexcept
{
    using namespace parser::keywords::operators;
    return op == Plus || op == Minus || op == Star || op == Slash || op == Percent || op == StarStar;
}

[[nodiscard]] inline bool IsBitwiseOpSym(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.tokPipe || sym == gs.tokAmp || sym == gs.tokCaret || sym == gs.tokLtLt || sym == gs.tokGtGt ||
           sym == gs.tokGtGtGt;
}

[[nodiscard]] inline bool IsArithmeticOpSym(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.tokPlus || sym == gs.tokDash || sym == gs.tokStar || sym == gs.tokSlash || sym == gs.tokPercent ||
           sym == gs.tokStarStar;
}

std::pair<std::string_view, std::string_view> GetOpMethods(std::string_view op)
{
    using namespace parser::keywords::operators;
    if (op == Pipe)
        return {"opOr", "opOr_r"};
    if (op == Amp)
        return {"opAnd", "opAnd_r"};
    if (op == Caret)
        return {"opXor", "opXor_r"};
    if (op == LtLt)
        return {"opShl", "opShl_r"};
    if (op == GtGt)
        return {"opShr", "opShr_r"};
    if (op == GtGtGt)
        return {"opUShr", "opUShr_r"};
    if (op == Plus)
        return {"opAdd", "opAdd_r"};
    if (op == Minus)
        return {"opSub", "opSub_r"};
    if (op == Star)
        return {"opMul", "opMul_r"};
    if (op == Slash)
        return {"opDiv", "opDiv_r"};
    if (op == Percent)
        return {"opMod", "opMod_r"};
    if (op == StarStar)
        return {"opPow", "opPow_r"};
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

static bool HasClassNumericOpImplConv(const std::string& classType, const std::string& otherType,
                                      const SymbolTable& table)
{
    if (TypeHasOpImplConvTo(classType, otherType, table))
    {
        return true;
    }
    if (!parser::primitives::IsNumeric(otherType))
    {
        return false;
    }
    return TypeHasOpImplConvTo(classType, "int", table) || TypeHasOpImplConvTo(classType, "double", table) ||
           TypeHasOpImplConvTo(classType, "float", table);
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
    const bool leftIsClass = !parser::primitives::IsPrimitive(types.left) && !ResolvesToEnum(types.left, table);
    const bool rightIsClass = !parser::primitives::IsPrimitive(types.right) && !ResolvesToEnum(types.right, table);
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
    if (types.left == "void" || types.right == "void")
    {
        return false;
    }
    if (!IsKnownType(types.left, table, stringTypeName) || !IsKnownType(types.right, table, stringTypeName))
    {
        return true;
    }
    if (types.left == "auto" || types.right == "auto" || types.left.find('?') != std::string::npos ||
        types.right.find('?') != std::string::npos)
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
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
    TSNode left = parser::GetChildByField(node, gs.fieldLeft);
    TSNode right = parser::GetChildByField(node, gs.fieldRight);
    if (ts_node_is_null(opNode) || ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const TSSymbol opSym = ts_node_symbol(opNode);
    if (!IsBitwiseOpSym(opSym, gs) && !IsArithmeticOpSym(opSym, gs))
    {
        return;
    }
    const std::string_view op = GetNodeTextView(opNode, ctx.request.sourceCode);

    if (const auto dt = IsBareDataType(left, scope, ctx.request.symbolTable, ctx.request.sourceCode))
    {
        const TSPoint start = ts_node_start_point(left);
        const TSPoint end = ts_node_end_point(left);
        ctx.EmitAtRange(SourceRange{start.row, start.column, end.row, end.column},
                        diagnostics::codes::ExpressionIsDataType, *dt);
        return;
    }
    if (const auto dt = IsBareDataType(right, scope, ctx.request.symbolTable, ctx.request.sourceCode))
    {
        const TSPoint start = ts_node_start_point(right);
        const TSPoint end = ts_node_end_point(right);
        ctx.EmitAtRange(SourceRange{start.row, start.column, end.row, end.column},
                        diagnostics::codes::ExpressionIsDataType, *dt);
        return;
    }
    if (IsFunctionReference(left, scope, ctx.request.symbolTable, ctx.request.sourceCode) ||
        IsFunctionReference(right, scope, ctx.request.symbolTable, ctx.request.sourceCode))
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

    if (!AreBinaryOperandsCompatible(op, *types, ctx.request.symbolTable, ctx.request.GetEffectiveStringTypeName()))
    {
        const TSPoint start = ts_node_start_point(opNode);
        const TSPoint end = ts_node_end_point(opNode);
        ctx.EmitAtRange(SourceRange{start.row, start.column, end.row, end.column},
                        diagnostics::codes::NoMatchingOperator, {std::string_view(op), types->left, types->right});
    }
}
} // namespace angel_lsp::analysis
