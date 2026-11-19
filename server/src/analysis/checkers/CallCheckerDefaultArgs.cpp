#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>
#include <variant>

namespace angel_lsp::analysis
{

static bool IsDefaultArgumentTypeMismatch(const ParameterInformation& param, const SymbolTable& table)
{
    if (param.defaultValue.empty())
    {
        return false;
    }
    std::string_view dv = param.defaultValue;
    TrimTypeWhitespace(dv);
    if (dv.empty())
    {
        return false;
    }
    if (parser::primitives::IsVoidType(dv))
    {
        return param.modifier != ParameterModifier::Out;
    }

    const std::string_view rawBase =
        CleanBaseTypeView(param.baseTypeName.empty() ? param.typeName : param.baseTypeName);
    const std::string paramBase = UnwrapTypedef(rawBase, table);
    if (paramBase.empty() || parser::primitives::IsAutoType(paramBase) || param.typeKind == TypeKind::VarType ||
        IsVariableTypeToken(paramBase) || param.isArray || !param.templateName.empty())
    {
        return false;
    }

    const bool isStringLit = dv.size() >= 2 && dv.front() == '"' && dv.back() == '"';
    const bool isBoolLit = (dv == parser::keywords::True || dv == parser::keywords::False);
    const bool isNullLit = (parser::primitives::IsNullType(dv));
    const bool isEnum = ResolvesToEnum(paramBase, table);

    return (isStringLit && (IsNumericPrimitive(paramBase) || parser::primitives::IsBoolType(paramBase) || isEnum)) ||
           (isBoolLit && (IsNumericPrimitive(paramBase) || isEnum || param.isHandle)) ||
           (isNullLit && !param.isHandle && (IsCorePrimitive(paramBase) || isEnum));
}

static bool MatchesScopedSuffix(std::string_view full, std::string_view prefix, std::string_view suffix) noexcept
{
    return full.size() == prefix.size() + 2 + suffix.size() && full.starts_with(prefix) &&
           full.substr(prefix.size(), 2) == "::" && full.ends_with(suffix);
}

static bool IsEnumMemberVisibleInScope(std::string_view memberName, std::string_view enclosingScope,
                                       std::string_view explicitEnumScope, const DiagnosticContext& ctx)
{
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    const auto it = ruleIndex.enumSymbolsByMemberName.find(memberName);
    if (it == ruleIndex.enumSymbolsByMemberName.end())
    {
        return false;
    }
    for (const auto& enumSym : it->second)
    {
        const std::string_view enumFull =
            enumSym.qualifiedName.empty() ? std::string_view(enumSym.name) : std::string_view(enumSym.qualifiedName);
        if (!explicitEnumScope.empty())
        {
            if (enumFull == explicitEnumScope)
            {
                return true;
            }
            if (!enclosingScope.empty() && MatchesScopedSuffix(enumFull, enclosingScope, explicitEnumScope))
            {
                return true;
            }
        }
        else if (enumSym.containerName.empty() || enumSym.containerName == enclosingScope)
        {
            return true;
        }
    }
    return false;
}

static bool DoesScopedDefaultRefResolve(std::string_view refName, std::string_view enclosingContainer,
                                        const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    const std::string_view clean = utils::StripGlobalPrefix(refName);
    if (table.FindSymbolsPtr(clean) != nullptr)
    {
        return true;
    }

    const std::string_view parentScope = utils::ParentScopeView(clean);
    const std::string_view memberName = utils::LastScopeSegment(clean);
    if (!parentScope.empty() && (table.FindMemberSymbolPtr(parentScope, memberName) != nullptr ||
                                 IsEnumMemberVisibleInScope(memberName, std::string_view{}, parentScope, ctx)))
    {
        return true;
    }

    if (utils::HasLeadingGlobalPrefix(refName) || enclosingContainer.empty())
    {
        return false;
    }

    bool found = false;
    utils::ForEachEnclosingScope(
        enclosingContainer,
        [&](std::string_view scope)
        {
            if (!ruleIndex.byNamespace.contains(scope))
            {
                return false;
            }
            if (table.HasScopedSymbol(scope, clean) ||
                (!parentScope.empty() && IsEnumMemberVisibleInScope(memberName, scope, parentScope, ctx)))
            {
                found = true;
                return true;
            }
            return false;
        });
    return found;
}

static bool DoesUnqualifiedDefaultRefResolve(std::string_view refName, const Symbol& candidate,
                                             const DiagnosticContext& ctx)
{
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    if (ruleIndex.globalNames.contains(refName))
    {
        return true;
    }

    const bool reqAccessorKw = ctx.request.RequiresAccessorKeyword();
    const auto& globalAccessors =
        reqAccessorKw ? ruleIndex.keywordGlobalAccessorPropertyNames : ruleIndex.globalAccessorPropertyNames;
    if (globalAccessors.contains(refName))
    {
        return true;
    }

    const bool allowUnqualifiedEnum = !ctx.request.RequiresEnumScope();
    if (allowUnqualifiedEnum && IsEnumMemberVisibleInScope(refName, std::string_view{}, std::string_view{}, ctx))
    {
        return true;
    }

    if (!candidate.containerName.empty())
    {
        bool foundInNs = false;
        utils::ForEachEnclosingScope(
            candidate.containerName,
            [&](std::string_view scope)
            {
                if (!ruleIndex.byNamespace.contains(scope))
                {
                    return false;
                }
                const auto& nsMembers = ruleIndex.NamespaceMembers(scope);
                const auto& nsAccessors =
                    reqAccessorKw ? nsMembers.keywordAccessorPropertyNames : nsMembers.accessorPropertyNames;
                if (nsMembers.allMemberNames.contains(refName) || nsAccessors.contains(refName) ||
                    (allowUnqualifiedEnum && IsEnumMemberVisibleInScope(refName, scope, std::string_view{}, ctx)))
                {
                    foundInNs = true;
                    return true;
                }
                return false;
            });
        if (foundInNs)
        {
            return true;
        }
    }

    if (candidate.fileUri == ctx.request.fileUri)
    {
        for (const auto& uNs : GetUsingNamespaces(ctx.request))
        {
            const auto& nsMembers = ruleIndex.NamespaceMembers(uNs);
            const auto& nsAccessors =
                reqAccessorKw ? nsMembers.keywordAccessorPropertyNames : nsMembers.accessorPropertyNames;
            if (nsMembers.allMemberNames.contains(refName) || nsAccessors.contains(refName) ||
                (allowUnqualifiedEnum && IsEnumMemberVisibleInScope(refName, uNs, std::string_view{}, ctx)))
            {
                return true;
            }
        }
    }
    return false;
}

static bool DoesDefaultArgRefResolve(const DefaultArgSymbolRef& ref, const Symbol& candidate,
                                     const DiagnosticContext& ctx)
{
    if (ref.name == parser::keywords::This)
    {
        return false;
    }
    if (ref.isScoped)
    {
        return DoesScopedDefaultRefResolve(ref.name, candidate.containerName, ctx);
    }
    return DoesUnqualifiedDefaultRefResolve(ref.name, candidate, ctx);
}

static bool CheckOmittedParameterDefault(const Symbol& candidate, size_t paramIndex, const CallSiteFact& site,
                                         DiagnosticContext& ctx)
{
    const auto& sig = candidate.GetFunction();
    if (paramIndex >= sig.parameters.size())
    {
        return false;
    }
    if (IsDefaultArgumentTypeMismatch(sig.parameters[paramIndex], ctx.request.symbolTable))
    {
        ctx.EmitAtRange(site.range, diagnostics::codes::DefaultArgTypeMismatch);
        return true;
    }
    for (const auto& ref : sig.defaultArgRefs)
    {
        if (ref.paramIndex == paramIndex && !DoesDefaultArgRefResolve(ref, candidate, ctx))
        {
            const std::string indexStr = std::to_string(paramIndex);
            ctx.EmitAtRange(site.range, diagnostics::codes::DefaultArgUnresolved, {indexStr, candidate.name, ref.name});
            return true;
        }
    }
    return false;
}

void CheckOmittedDefaultArguments(const Symbol& candidate, size_t providedPositionalCount, const CallSiteFact& site,
                                  DiagnosticContext& ctx)
{
    if (candidate.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(candidate.signature) ||
        IsFromPredefinedStub(candidate, ctx))
    {
        return;
    }
    const auto& sig = candidate.GetFunction();
    for (size_t p = providedPositionalCount; p < sig.parameters.size(); ++p)
    {
        if (CheckOmittedParameterDefault(candidate, p, site, ctx))
        {
            return;
        }
    }
}

void CheckOmittedNamedDefaultArguments(const Symbol& candidate,
                                       const ankerl::unordered_dense::set<size_t>& matchedParams,
                                       const CallSiteFact& site, DiagnosticContext& ctx)
{
    if (candidate.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(candidate.signature) ||
        IsFromPredefinedStub(candidate, ctx))
    {
        return;
    }
    const auto& sig = candidate.GetFunction();
    for (size_t p = 0; p < sig.parameters.size(); ++p)
    {
        if (!matchedParams.contains(p) && CheckOmittedParameterDefault(candidate, p, site, ctx))
        {
            return;
        }
    }
}

} // namespace angel_lsp::analysis
