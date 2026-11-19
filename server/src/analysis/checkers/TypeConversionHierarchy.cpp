#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static std::string_view StripTemplateOrBaseTypeView(std::string_view typeName)
{
    const std::string_view stripped = StripTrailingDecorationsView(StripLeadingConst(typeName));
    const auto tmpl = DecomposeTemplateType(stripped);
    if (tmpl.isTemplate && !tmpl.containerName.empty())
    {
        return tmpl.containerName;
    }
    if (stripped.empty())
    {
        return typeName;
    }
    return stripped;
}

void ForEachMethod(std::string_view typeName, std::string_view memberName, const SymbolTable& table,
                   const std::function<bool(const Symbol&)>& visitor)
{
    bool stopped = false;
    const std::string_view lookupType = StripTemplateOrBaseTypeView(typeName);
    for (const auto& cls : GetInheritedTypeHierarchy(lookupType, table))
    {
        ForEachMemberSymbolNamed(cls, memberName, table,
                                 [&](const Symbol& sym)
                                 {
                                     if (sym.type != SymbolType::Function)
                                     {
                                         return false;
                                     }
                                     stopped = visitor(sym);
                                     return stopped;
                                 });

        if (stopped)
        {
            return;
        }
    }
}

void ForEachConstructor(std::string_view typeName, const SymbolTable& table,
                        const std::function<bool(const Symbol&)>& visitor)
{
    const size_t open = TemplateArgumentsStart(typeName);
    const auto unparameterized = (open == std::string_view::npos) ? typeName : typeName.substr(0, open);

    const auto bare = LastScopeSegment(unparameterized);
    bool sawAny = false;
    bool stopped = false;

    const auto visit = [&](const Symbol& sym)
    {
        if (sym.type != SymbolType::Function)
        {
            return false;
        }
        sawAny = true;
        stopped = visitor(sym);
        return stopped;
    };

    ForEachMemberSymbolNamed(unparameterized, bare, table, visit);
    if (!stopped && !sawAny && bare != unparameterized)
    {
        ForEachMemberSymbolNamed(bare, bare, table, visit);
    }

    if (!stopped && !sawAny)
    {
        auto shortMatches = table.FindTypeSymbolsByShortName(bare);
        for (const auto& cSym : shortMatches)
        {
            if (cSym.type == SymbolType::Class)
            {
                const auto qCls =
                    cSym.qualifiedName.empty() ? std::string_view{cSym.name} : std::string_view{cSym.qualifiedName};
                ForEachMemberSymbolNamed(qCls, bare, table, visit);
                if (stopped || sawAny)
                {
                    break;
                }
            }
        }
    }
}

bool AcceptsSingleArgument(const std::vector<ParameterInformation>& parameters)
{
    if (parameters.empty())
    {
        return false;
    }
    return std::all_of(parameters.begin() + 1, parameters.end(),
                       [](const ParameterInformation& param) { return !param.defaultValue.empty(); });
}

std::string_view AssignmentOverloadName(std::string_view op)
{
    if (op == "=")
        return "opAssign";
    if (op == "+=")
        return "opAddAssign";
    if (op == "-=")
        return "opSubAssign";
    if (op == "*=")
        return "opMulAssign";
    if (op == "/=")
        return "opDivAssign";
    if (op == "%=")
        return "opModAssign";
    if (op == "**=")
        return "opPowAssign";
    if (op == "&=")
        return "opAndAssign";
    if (op == "|=")
        return "opOrAssign";
    if (op == "^=")
        return "opXorAssign";
    if (op == "<<=")
        return "opShlAssign";
    if (op == ">>=")
        return "opShrAssign";
    if (op == ">>>=")
        return "opUShrAssign";
    return {};
}

bool DeclaresOperatorMethod(std::string_view typeName, std::string_view methodName, const SymbolTable& table)
{
    if (typeName.empty() || methodName.empty())
    {
        return false;
    }
    bool found = false;
    ForEachMethod(typeName, methodName, table,
                  [&](const Symbol&)
                  {
                      found = true;
                      return true;
                  });
    return found;
}

/**
 * @brief Tells whether a value can be converted to the type with a plain scalar cast.
 * @param[in] typeName Target type spelling.
 * @param[in] table Symbol table used to recognise enums.
 * @return True for primitive types and enums.
 */
bool IsScalarCastTarget(std::string_view typeName, const SymbolTable& table)
{
    if (IsPrimitiveTypeName(typeName))
    {
        return true;
    }

    const auto symbols = table.FindSymbolsPtr(typeName);
    return symbols && std::any_of(symbols->begin(), symbols->end(),
                                  [](const Symbol& sym) { return sym.type == SymbolType::Enum; });
}

bool DeclaresAnyCastOperator(std::string_view typeName, const SymbolTable& table)
{
    bool found = false;
    const auto mark = [&found](const Symbol&)
    {
        found = true;
        return true;
    };

    ForEachMethod(typeName, "opCast", table, mark);
    if (!found)
    {
        ForEachMethod(typeName, "opImplCast", table, mark);
    }
    return found;
}

bool DeclaresConversionTo(std::string_view fromType, std::string_view toType, const SymbolTable& table,
                          bool implicitOnly)
{
    bool found = false;
    TemplateBinding binding;
    if (HasTemplateArgumentsView(fromType))
    {
        binding = BindTemplateArguments(fromType, table);
    }
    const std::string_view strippedTo = StripTrailingDecorationsView(StripLeadingConst(toType));
    const bool toIsTemplate = DecomposeTemplateType(strippedTo).isTemplate;
    const bool toIsNumeric = IsNumericPrimitive(toType);
    const auto matches = [&](const Symbol& sym)
    {
        if (!sym.GetFunction().parameters.empty())
        {
            return false;
        }
        auto ret = sym.GetFunction().returnType;
        if (binding.usable)
        {
            ret = SubstituteTemplateParameters(ret, binding);
        }
        const std::string_view strippedRet = StripTrailingDecorationsView(StripLeadingConst(ret));
        const bool eitherTemplate = toIsTemplate || DecomposeTemplateType(strippedRet).isTemplate;
        const bool sameType = eitherTemplate
                                  ? IsSameType(strippedRet, strippedTo)
                                  : (IsSameType(strippedRet, strippedTo) || IsSameType(CleanBaseType(ret), toType));
        if (sameType || (toIsNumeric && (IsNumericPrimitive(strippedRet) || ResolvesToEnum(strippedRet, table))))
        {
            found = true;
            return true;
        }
        return false;
    };

    static const char* k_implicit[] = {"opImplConv", "opImplCast"};
    static const char* k_explicit[] = {"opConv", "opCast"};

    for (const char* opName : k_implicit)
    {
        ForEachMethod(fromType, opName, table, matches);
        if (found)
        {
            return true;
        }
    }
    if (implicitOnly)
    {
        return false;
    }
    for (const char* opName : k_explicit)
    {
        ForEachMethod(fromType, opName, table, matches);
        if (found)
        {
            return true;
        }
    }
    return false;
}

bool AreHierarchyRelated(std::string_view a, std::string_view b, const SymbolTable& table)
{
    const std::string_view strippedA = StripTrailingDecorationsView(StripLeadingConst(a));
    const std::string_view strippedB = StripTrailingDecorationsView(StripLeadingConst(b));
    if (DecomposeTemplateType(strippedA).isTemplate || DecomposeTemplateType(strippedB).isTemplate)
    {
        return IsSameType(strippedA, strippedB);
    }
    const auto cleanA = CleanBaseType(a);
    const auto cleanB = CleanBaseType(b);
    for (const auto& base : GetInheritedTypeHierarchy(cleanA, table))
    {
        if (IsSameType(base, cleanB))
        {
            return true;
        }
    }
    for (const auto& base : GetInheritedTypeHierarchy(cleanB, table))
    {
        if (IsSameType(base, cleanA))
        {
            return true;
        }
    }
    return false;
}

bool IsStringType(std::string_view typeName, const DiagnosticContext& ctx)
{
    return typeName == ctx.request.GetEffectiveStringTypeName();
}

bool IsBuiltInValueType(std::string_view typeName, const DiagnosticContext& ctx)
{
    return IsPrimitiveTypeName(typeName) || IsStringType(typeName, ctx);
}

bool HasEmptyType(std::string_view from, std::string_view to)
{
    return from.empty() || to.empty();
}

} // namespace angel_lsp::analysis
