#include "analysis/checkers/BinaryOperatorHelpers.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/OperandTypePair.h"
#include "analysis/types/TypeSanitization.h"

#include <algorithm>
#include <optional>
#include <string_view>

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
    const auto cleanParam = CleanBaseType(param.typeName);
    if (cleanParam == "?" || cleanParam == "?&" || param.typeName.find('?') != std::string_view::npos)
    {
        return true;
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
            if (sym.type == SymbolType::Function && !sym.GetFunction().parameters.empty() &&
                IsParameterCompatible(sym.GetFunction().parameters.front(), argType, table))
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
    if (type == "null" || IsPrimitiveTypeName(type) || type == stringTypeName || ResolvesToEnum(type, table))
    {
        return true;
    }
    if (type.ends_with("[]"))
    {
        return true;
    }
    const auto templatePos = type.find('<');
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
    return type == "null" || operand.isNullLiteral;
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
 * @brief Resolves both operand types of a binary expression and strips their modifiers.
 * @param[in] fact Binary expression facts.
 * @param[in] scope Lexical scope enclosing the expression.
 * @param[in] ctx Diagnostic context.
 * @return Clean operand types, or nullopt when either is unresolved or poisoned.
 */
std::optional<OperandTypePair> ResolveCleanBinaryOperandTypes(const BinaryExpressionFact& fact, const Scope* scope,
                                                              const DiagnosticContext& ctx)
{
    const ExpressionTypeContext exprCtx(scope, ctx);
    const auto rawLeft = ResolveExpressionType(fact.left.bytes, exprCtx);
    const auto rawRight = ResolveExpressionType(fact.right.bytes, exprCtx);
    if (rawLeft.empty() || rawRight.empty())
    {
        return std::nullopt;
    }
    if (ExpressionTypeCache::IsPoisonType(rawLeft) || ExpressionTypeCache::IsPoisonType(rawRight))
    {
        return std::nullopt;
    }
    const auto cleanLeft = IsNullOperand(fact.left, rawLeft) ? "null" : StripModifiers(rawLeft);
    const auto cleanRight = IsNullOperand(fact.right, rawRight) ? "null" : StripModifiers(rawRight);
    if (cleanLeft.empty() || cleanRight.empty())
    {
        return std::nullopt;
    }
    return MakeOperandTypePair(cleanLeft, cleanRight);
}

} // namespace angel_lsp::analysis
