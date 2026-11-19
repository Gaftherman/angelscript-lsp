#include "analysis/checkers/BinaryOperatorHelpers.h"

#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/OperandTypePair.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace angel_lsp::analysis
{

/**
 * @brief Tests whether a parameter accepts an argument of the given type.
 * @param[in] param Declared parameter.
 * @param[in] argType Actual argument type name.
 * @param[in] table Symbol table used to resolve conversions.
 * @return True when the argument can bind to the parameter.
 */
bool IsParameterCompatible(const ParameterInformation& param, std::string_view argType, const SymbolTable& table)
{
    if (param.typeKind == TypeKind::VarType || IsVariableTypeToken(param.typeName))
    {
        return true;
    }
    const auto cleanParam = CleanBaseType(param.typeName);
    if (IsVariableTypeToken(cleanParam))
    {
        return true;
    }
    if (parser::primitives::IsNullType(argType))
    {
        return param.modifier != ParameterModifier::Out && param.modifier != ParameterModifier::InOut &&
               IsHandleType(param.typeName, table);
    }
    if (cleanParam == argType || param.typeName == argType)
    {
        return true;
    }
    if (IsNumericPrimitive(cleanParam) && IsNumericPrimitive(argType))
    {
        return true;
    }
    const auto hierarchy = GetInheritedTypeHierarchy(argType, table);
    return std::find(hierarchy.begin(), hierarchy.end(), cleanParam) != hierarchy.end();
}

/**
 * @brief Validates the return type shape required for comparison operator overloads.
 * @param[in] opName Operator method name being checked.
 * @param[in] sig Candidate operator overload signature.
 * @return True when opName is not a comparison operator, or when its return type matches AngelScript's requirement.
 */
static bool IsValidComparisonOperatorReturn(std::string_view opName, const FunctionSignature& sig) noexcept
{
    if (opName == "opEquals")
    {
        return sig.returnTypeKind == TypeKind::Bool && !sig.returnIsArray && !sig.modifiers.isHandle &&
               !sig.returnIsConst && !sig.modifiers.isReturnReference;
    }
    if (opName == "opCmp")
    {
        return sig.returnTypeKind == TypeKind::Int32 && !sig.returnIsArray && !sig.modifiers.isHandle &&
               !sig.returnIsConst && !sig.modifiers.isReturnReference;
    }
    return true;
}

/**
 * @brief Tests whether a type declares or inherits an operator accepting the argument type.
 * @param[in] typeName Type that owns the operator.
 * @param[in] opName Operator method name such as opAdd.
 * @param[in] argType Argument type passed to the operator.
 * @param[in] table Symbol table to search.
 * @return True when a compatible overload exists.
 */
bool TypeHasOperator(std::string_view typeName, std::string_view opName, std::string_view argType,
                     const SymbolTable& table)
{
    if (typeName.empty() || IsPrimitiveTypeName(typeName))
    {
        return false;
    }
    for (const auto& cls : GetInheritedTypeHierarchy(typeName, table))
    {
        const auto symbols = table.FindMemberSymbolPtr(cls, opName);
        if (!symbols)
        {
            continue;
        }
        for (const auto& sym : *symbols)
        {
            if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
            {
                continue;
            }
            const auto& sig = sym.GetFunction();
            if (sig.parameters.size() != 1 || !IsValidComparisonOperatorReturn(opName, sig))
            {
                continue;
            }
            if (IsParameterCompatible(sig.parameters.front(), argType, table))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Tests whether a type converts implicitly to a target through opImplConv or opConv.
 * @param[in] typeName Source type.
 * @param[in] targetType Destination type.
 * @param[in] table Symbol table to search.
 * @return True when a conversion operator to the target exists.
 */
bool TypeHasOpImplConvTo(std::string_view typeName, std::string_view targetType, const SymbolTable& table)
{
    if (typeName.empty() || IsPrimitiveTypeName(typeName))
    {
        return false;
    }
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

/**
 * @brief Tests whether a type name is declared or built in.
 * @param[in] type Type spelling.
 * @param[in] table Symbol table to search.
 * @param[in] stringTypeName Configured string type name.
 * @return True when the type is known.
 */
bool IsKnownType(std::string_view type, const SymbolTable& table, std::string_view stringTypeName)
{
    if (parser::primitives::IsNullType(type) || IsPrimitiveTypeName(type) || type == stringTypeName ||
        ResolvesToEnum(type, table))
    {
        return true;
    }
    if (type.ends_with("[]"))
    {
        return true;
    }
    const auto templatePos = TemplateArgumentsStart(type);
    if (templatePos != std::string_view::npos)
    {
        const std::string_view baseTemplate = type.substr(0, templatePos);
        const auto symbols = table.FindSymbolsPtr(baseTemplate);
        if (symbols && !symbols->empty())
        {
            return true;
        }
    }
    const auto symbols = table.FindSymbolsPtr(type);
    return symbols && !symbols->empty();
}

/**
 * @brief Tests whether an operand is the null literal or resolved to null.
 * @param[in] operand Operand facts.
 * @param[in] type Resolved operand type spelling.
 * @return True when the operand is null.
 */
bool IsNullOperand(const BinaryOperandFact& operand, std::string_view type)
{
    return parser::primitives::IsNullType(type) || operand.isNullLiteral;
}

/**
 * @brief Strips leading const, trailing decorations, and type whitespace to obtain a bare base type.
 * @param[in] type Raw type name string view.
 * @return Sanitized base type name string view.
 */
static std::string_view StripModifiers(std::string_view type)
{
    type = StripLeadingConst(type);
    type = StripTrailingDecorationsView(type);
    TrimTypeWhitespace(type);
    return type;
}

/**
 * @brief Resolves both operand types of a binary expression, retaining handle/null flags alongside stripped types.
 * @param[in] fact Binary expression facts.
 * @param[in] scope Lexical scope enclosing the expression.
 * @param[in] ctx Diagnostic context.
 * @return Resolved operand bundle, or nullopt when either operand is unresolved or poisoned.
 */
std::optional<ResolvedBinaryOperands> ResolveBinaryOperands(const BinaryExpressionFact& fact, const Scope* scope,
                                                            const DiagnosticContext& ctx)
{
    const ExpressionTypeContext exprCtx(scope, ctx);
    const auto rawLeft = ResolveExpressionType(fact.left.bytes, exprCtx);
    const auto rawRight = ResolveExpressionType(fact.right.bytes, exprCtx);
    if (rawLeft.empty() || rawRight.empty())
    {
        return std::nullopt;
    }
    const bool leftIsNull = IsNullOperand(fact.left, rawLeft);
    const bool rightIsNull = IsNullOperand(fact.right, rawRight);
    const auto cleanLeft = leftIsNull ? std::string_view(parser::keywords::Null) : StripModifiers(rawLeft);
    const auto cleanRight = rightIsNull ? std::string_view(parser::keywords::Null) : StripModifiers(rawRight);
    if (cleanLeft.empty() || cleanRight.empty())
    {
        return std::nullopt;
    }
    const auto& table = ctx.request.symbolTable;
    return ResolvedBinaryOperands{
        .clean = MakeOperandTypePair(cleanLeft, cleanRight),
        .leftIsHandle =
            !leftIsNull && (IsHandleType(rawLeft, table) || fact.left.isThis || fact.left.hasHandleOperator),
        .rightIsHandle =
            !rightIsNull && (IsHandleType(rawRight, table) || fact.right.isThis || fact.right.hasHandleOperator),
        .leftIsNull = leftIsNull,
        .rightIsNull = rightIsNull,
    };
}

/**
 * @brief Resolves both operand types of a binary expression and strips their modifiers.
 * @param[in] fact Binary expression facts.
 * @param[in] scope Lexical scope enclosing the expression.
 * @param[in] ctx Diagnostic context.
 * @return Clean operand types, or nullopt when either is unresolved or poisoned.
 */
std::optional<OperandTypePair> ResolveCleanBinaryOperandTypes(const BinaryExpressionFact& fact, const Scope* scope,
                                                              const DiagnosticContext& ctx)
{
    auto resolved = ResolveBinaryOperands(fact, scope, ctx);
    if (!resolved)
    {
        return std::nullopt;
    }
    return std::move(resolved->clean);
}

} // namespace angel_lsp::analysis
