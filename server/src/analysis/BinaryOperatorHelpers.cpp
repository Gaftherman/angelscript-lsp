#include "analysis/BinaryOperatorHelpers.h"
#include "analysis/ExpressionTypeCache.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "analysis/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
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

bool IsKnownType(std::string_view type, const SymbolTable& table, std::string_view stringTypeName)
{
    if (type == "null" || parser::primitives::IsPrimitive(type) || type == stringTypeName ||
        ResolvesToEnum(type, table))
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

bool IsNullOperand(TSNode node, std::string_view type)
{
    if (type == parser::keywords::Null || IsNullInitializer(node))
    {
        return true;
    }
    return !ts_node_is_null(node) && ts_node_symbol(node) == parser::GetGrammarSymbols().symNullLiteral;
}

/**
 * @brief Strip modifiers.
 * @param[in] type Parameter type.
 * @return Result of the operation.
 * @note [REFACTOR-CANDIDATE] Structural overlap (68%) with CleanExpressionType in
 * server/src/analysis/SemanticHelpers.cpp. Candidate for unification in server/src/analysis/SemanticHelpers.h. Manually
 * re-evaluates AST for isConst; candidate for direct Pass 1 flag read via TypeExtraction::ExtractTypeInfo /
 * Symbol::modifiers.isConst. Manually re-evaluates AST for isHandle; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isHandle / LocalDefinition::isHandleType.
 */
static std::string StripModifiers(std::string_view type)
{
    type = StripLeadingConst(type);
    type = StripTrailingDecorationsView(type);
    TrimTypeWhitespace(type);
    return std::string(type);
}

std::optional<BinaryOperandTypes> ResolveCleanBinaryOperandTypes(TSNode left, TSNode right, const Scope* scope,
                                                                 const DiagnosticContext& ctx)
{
    const ExpressionTypeContext exprCtx(scope, ctx);
    const std::string rawLeft = ResolveExpressionType(left, exprCtx);
    const std::string rawRight = ResolveExpressionType(right, exprCtx);
    if (rawLeft.empty() || rawRight.empty())
    {
        return std::nullopt;
    }
    if (ExpressionTypeCache::IsPoisonType(rawLeft) || ExpressionTypeCache::IsPoisonType(rawRight))
    {
        return std::nullopt;
    }
    std::string cleanLeft = IsNullOperand(left, rawLeft) ? "null" : StripModifiers(rawLeft);
    std::string cleanRight = IsNullOperand(right, rawRight) ? "null" : StripModifiers(rawRight);
    if (cleanLeft.empty() || cleanRight.empty())
    {
        return std::nullopt;
    }
    return BinaryOperandTypes{std::move(cleanLeft), std::move(cleanRight)};
}
} // namespace angel_lsp::analysis
