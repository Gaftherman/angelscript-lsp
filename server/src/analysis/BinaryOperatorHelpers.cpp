#include "analysis/BinaryOperatorHelpers.h"
#include "analysis/ASTUtils.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "parser/Primitives.h"

#include <algorithm>

namespace angel_lsp::analysis
{
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
    if (parser::primitives::IsNumeric(cleanParam) && parser::primitives::IsNumeric(argType))
    {
        return true;
    }
    const auto hierarchy = GetInheritedTypeHierarchy(argType, table);
    return std::find(hierarchy.begin(), hierarchy.end(), cleanParam) != hierarchy.end();
}

bool TypeHasOperator(const std::string& typeName, const std::string& opName, const std::string& argType,
                     const SymbolTable& table)
{
    if (typeName.empty() || parser::primitives::IsPrimitive(typeName))
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

bool TypeHasOpImplConvTo(const std::string& typeName, const std::string& targetType, const SymbolTable& table)
{
    if (typeName.empty() || parser::primitives::IsPrimitive(typeName))
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

bool IsKnownType(const std::string& type, const SymbolTable& table, std::string_view stringTypeName)
{
    if (parser::primitives::IsNumeric(type) || type == "bool" || type == stringTypeName ||
        ResolvesToEnum(type, table))
    {
        return true;
    }
    const auto symbols = table.FindSymbolsPtr(type);
    return symbols && !symbols->empty();
}

bool IsNullOperand(TSNode node, const std::string& type)
{
    return type.empty() || type == "null" || IsNullInitializer(node);
}

std::optional<BinaryOperandTypes> ResolveCleanBinaryOperandTypes(TSNode left, TSNode right, const Scope* scope,
                                                                 const DiagnosticContext& ctx)
{
    const ExpressionTypeContext exprCtx(scope, ctx);
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
    return BinaryOperandTypes{std::move(cleanLeft), std::move(cleanRight)};
}
} // namespace angel_lsp::analysis
