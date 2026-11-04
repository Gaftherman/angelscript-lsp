#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "parser/Keywords.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

void ForEachMethod(const std::string& typeName, const std::string& memberName, const SymbolTable& table,
                   const std::function<bool(const Symbol&)>& visitor)
{
    bool stopped = false;
    for (const auto& cls : GetInheritedTypeHierarchy(typeName, table))
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

void ForEachConstructor(const std::string& typeName, const SymbolTable& table,
                        const std::function<bool(const Symbol&)>& visitor)
{
    const size_t open = typeName.find('<');
    const std::string unparameterized = (open == std::string::npos) ? typeName : typeName.substr(0, open);

    const std::string bare{LastScopeSegment(unparameterized)};
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
                const std::string_view qCls = cSym.qualifiedName.empty() ? cSym.name : cSym.qualifiedName;
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

std::string SingleArgumentType(const std::vector<ParameterInformation>& parameters)
{
    return parameters.empty() ? "" : CanonicalizeType(parameters[0].typeName);
}

std::string_view AssignmentOverloadName(std::string_view op)
{
    if (op == parser::keywords::operators::Assign)
        return "opAssign";
    if (op == parser::keywords::operators::PlusAssign)
        return "opAddAssign";
    if (op == parser::keywords::operators::MinusAssign)
        return "opSubAssign";
    if (op == parser::keywords::operators::StarAssign)
        return "opMulAssign";
    if (op == parser::keywords::operators::SlashAssign)
        return "opDivAssign";
    if (op == parser::keywords::operators::PercentAssign)
        return "opModAssign";
    if (op == parser::keywords::operators::StarStarAssign)
        return "opPowAssign";
    if (op == parser::keywords::operators::AmpAssign)
        return "opAndAssign";
    if (op == parser::keywords::operators::PipeAssign)
        return "opOrAssign";
    if (op == parser::keywords::operators::CaretAssign)
        return "opXorAssign";
    if (op == parser::keywords::operators::LtLtAssign)
        return "opShlAssign";
    if (op == parser::keywords::operators::GtGtAssign)
        return "opShrAssign";
    if (op == parser::keywords::operators::GtGtGtAssign)
        return "opUShrAssign";
    return {};
}

bool DeclaresOperatorMethod(const std::string& typeName, std::string_view methodName, const SymbolTable& table)
{
    if (typeName.empty() || methodName.empty())
    {
        return false;
    }
    return table.FindMemberSymbolPtr(typeName, methodName) != nullptr;
}

bool IsScalarCastTarget(const std::string& typeName, const SymbolTable& table)
{
    if (parser::primitives::IsPrimitive(typeName))
    {
        return true;
    }

    const auto symbols = table.FindSymbolsPtr(typeName);
    return symbols && std::any_of(symbols->begin(), symbols->end(),
                                  [](const Symbol& sym) { return sym.type == SymbolType::Enum; });
}

bool DeclaresAnyCastOperator(const std::string& typeName, const SymbolTable& table)
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

bool DeclaresConversionTo(const std::string& fromType, const std::string& toType, const SymbolTable& table,
                          bool implicitOnly)
{
    bool found = false;
    TemplateBinding binding;
    if (fromType.find('<') != std::string::npos)
    {
        binding = BindTemplateArguments(fromType, table);
    }
    const auto matches = [&](const Symbol& sym)
    {
        std::string ret = sym.GetFunction().returnType;
        if (binding.usable)
        {
            ret = SubstituteTemplateParameters(ret, binding);
        }
        if (IsSameType(CleanBaseType(ret), toType))
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

bool IsDerivedFrom(const std::string& derived, const std::string& base, const SymbolTable& table)
{
    const std::string cleanDerived = CleanBaseType(derived);
    const std::string cleanBase = CleanBaseType(base);
    for (const auto& ancestor : GetInheritedTypeHierarchy(cleanDerived, table))
    {
        if (IsSameType(ancestor, cleanBase))
        {
            return true;
        }
    }
    return false;
}

bool AreHierarchyRelated(const std::string& a, const std::string& b, const SymbolTable& table)
{
    const std::string cleanA = CleanBaseType(a);
    const std::string cleanB = CleanBaseType(b);
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

bool IsStringType(const std::string& typeName, const DiagnosticContext& ctx)
{
    return typeName == ctx.request.GetEffectiveStringTypeName();
}

bool IsBuiltInValueType(const std::string& typeName, const DiagnosticContext& ctx)
{
    return IsPrimitiveTypeName(typeName) || IsStringType(typeName, ctx);
}

bool IsPoisonOrEmptyType(std::string_view from, std::string_view to)
{
    return from.empty() || to.empty() || ExpressionTypeCache::IsPoisonType(from) ||
           ExpressionTypeCache::IsPoisonType(to);
}

} // namespace angel_lsp::analysis
