#include "analysis/resolution/InterfaceImplementation.h"

#include "analysis/index/RuleIndex.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <span>

namespace angel_lsp::analysis
{
static std::string InterfaceSignatureType(std::string_view type, const Symbol& owner, const SymbolTable& table)
{
    return NormalizeType(QualifyTypeInContainer(type, owner.containerName, table), table.GetDefaultArrayType());
}

static bool MatchesInterfaceParameters(const Symbol& left, const Symbol& right, const SymbolTable& table)
{
    const auto& a = left.GetFunction();
    const auto& b = right.GetFunction();
    if (left.name != right.name || a.modifiers.isConst != b.modifiers.isConst ||
        a.parameters.size() != b.parameters.size())
    {
        return false;
    }

    for (size_t i = 0; i < a.parameters.size(); ++i)
    {
        const auto& aParameter = a.parameters[i];
        const auto& bParameter = b.parameters[i];
        if (aParameter.modifier != bParameter.modifier || aParameter.isHandle != bParameter.isHandle ||
            aParameter.isReference != bParameter.isReference || aParameter.isConst != bParameter.isConst ||
            InterfaceSignatureType(aParameter.typeName, left, table) !=
                InterfaceSignatureType(bParameter.typeName, right, table))
        {
            return false;
        }
    }
    return true;
}

static bool MatchesInterfaceReturn(const Symbol& left, const Symbol& right, const SymbolTable& table)
{
    const auto& a = left.GetFunction();
    const auto& b = right.GetFunction();
    return a.modifiers.isReturnReference == b.modifiers.isReturnReference &&
           a.modifiers.isHandle == b.modifiers.isHandle && a.returnIsConst == b.returnIsConst &&
           InterfaceSignatureType(a.returnType, left, table) == InterfaceSignatureType(b.returnType, right, table);
}

/**
 * @brief Accepts an inherited name only when it has an indexed interface declaration.
 */
static bool IsInterfaceContract(std::string_view name, const SymbolTable& table)
{
    const auto symbols = table.FindSymbolsPtr(name);
    return symbols && std::any_of(symbols->begin(), symbols->end(),
                                  [](const auto& symbol) { return symbol.type == SymbolType::Interface; });
}

static bool IsInterfaceMethodCandidate(const Symbol& symbol)
{
    return symbol.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(symbol.signature) &&
           !IsDestructorSymbol(symbol) && symbol.name != utils::LastScopeSegment(symbol.containerName);
}

static void ExpandVirtualPropertyAccessors(const Symbol& propSym, std::vector<Symbol>& outMethods)
{
    if (propSym.type != SymbolType::Property || !std::holds_alternative<VariableSignature>(propSym.signature))
    {
        return;
    }
    const auto& var = propSym.GetVariable();
    if (!var.isVirtualProperty)
    {
        return;
    }

    if (var.hasGet)
    {
        Symbol getter = propSym;
        getter.type = SymbolType::Function;
        getter.name = "get_" + propSym.name;
        FunctionSignature fn;
        fn.returnType = var.typeName;
        fn.returnBaseTypeName = var.baseTypeName;
        fn.returnIsConst = IsConstTypeView(var.typeName);
        fn.modifiers.access = var.modifiers.access;
        fn.modifiers.isHandle = var.modifiers.isHandle || IsHandleTypeView(var.typeName);
        fn.modifiers.isReturnReference = IsReferenceTypeView(var.typeName);
        fn.modifiers.isConst = var.isGetConst;
        fn.modifiers.isFinal = var.isGetFinal;
        fn.modifiers.isProperty = true;
        getter.signature = std::move(fn);
        outMethods.push_back(std::move(getter));
    }

    if (var.hasSet)
    {
        Symbol setter = propSym;
        setter.type = SymbolType::Function;
        setter.name = "set_" + propSym.name;
        FunctionSignature fn;
        fn.returnType = "void";
        fn.returnBaseTypeName = "void";
        fn.returnTypeKind = TypeKind::Void;
        fn.modifiers.access = var.modifiers.access;
        fn.modifiers.isFinal = var.isSetFinal;
        fn.modifiers.isProperty = true;
        ParameterInformation param;
        param.name = "value";
        param.typeName = var.typeName;
        param.baseTypeName = var.baseTypeName;
        param.isHandle = var.modifiers.isHandle || IsHandleTypeView(var.typeName);
        param.isReference = IsReferenceTypeView(var.typeName);
        param.isConst = IsConstTypeView(var.typeName);
        fn.parameters.push_back(std::move(param));
        setter.signature = std::move(fn);
        outMethods.push_back(std::move(setter));
    }
}

static bool EvaluateCandidateImplementation(const Symbol& candidate, const Symbol& method, bool isDirectOwner,
                                            const SymbolTable& table, bool& canOverride)
{
    if (!IsInterfaceMethodCandidate(candidate) || !MatchesInterfaceParameters(candidate, method, table))
    {
        return false;
    }
    const auto& fn = candidate.GetFunction();
    const bool returnMatches = MatchesInterfaceReturn(candidate, method, table);
    if (returnMatches && !fn.modifiers.isAbstract && !fn.modifiers.isDeclarationAbstract &&
        fn.modifiers.access == AccessModifier::Public)
    {
        return true;
    }
    if (!returnMatches || fn.modifiers.isFinal || fn.modifiers.isDeclarationFinal || isDirectOwner)
    {
        canOverride = false;
    }
    return false;
}

static bool HasInterfaceImplementation(std::span<const std::string> owners, const Symbol& method,
                                       const SymbolTable& table, bool& canOverride)
{
    for (const auto& owner : owners)
    {
        const bool isDirectOwner = (owner == owners.front());
        if (const auto candidates = table.FindMemberSymbolPtr(owner, method.name))
        {
            for (const auto& candidate : *candidates)
            {
                if (EvaluateCandidateImplementation(candidate, method, isDirectOwner, table, canOverride))
                {
                    return true;
                }
            }
        }
        if (method.name.size() > 4 && (method.name.starts_with("get_") || method.name.starts_with("set_")))
        {
            if (const auto props = table.FindMemberSymbolPtr(owner, std::string_view(method.name).substr(4)))
            {
                std::vector<Symbol> expanded;
                for (const auto& prop : *props)
                {
                    ExpandVirtualPropertyAccessors(prop, expanded);
                }
                for (const auto& accessor : expanded)
                {
                    if (EvaluateCandidateImplementation(accessor, method, isDirectOwner, table, canOverride))
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static void RecordMissingInterfaceMethod(const Symbol& method, std::string_view interfaceName,
                                         std::span<const std::string> owners, const SymbolTable& table,
                                         std::vector<MissingInterfaceMethod>& missing)
{
    bool canOverride = true;
    if (HasInterfaceImplementation(owners, method, table, canOverride))
    {
        return;
    }
    const auto duplicate = std::find_if(missing.begin(), missing.end(), [&](const auto& entry)
                                        { return MatchesInterfaceParameters(entry.method, method, table); });
    if (duplicate != missing.end())
    {
        duplicate->canOverride &= canOverride && MatchesInterfaceReturn(duplicate->method, method, table);
    }
    else
    {
        missing.push_back({method, std::string(interfaceName), canOverride});
    }
}

static void AppendMissingMethods(std::string_view interfaceName, std::span<const std::string> owners,
                                 const SymbolTable& table, std::vector<MissingInterfaceMethod>& missing)
{
    const auto index = table.GetRuleIndex();
    for (const auto& interfaceOwner : GetInheritedTypeHierarchy(interfaceName, table))
    {
        if (!IsInterfaceContract(interfaceOwner, table))
        {
            continue;
        }
        for (const auto& key : index->Members(interfaceOwner).memberKeys)
        {
            const auto methods = table.FindSymbolsPtr(key);
            if (!methods)
            {
                continue;
            }
            for (const auto& method : *methods)
            {
                if (method.containerName != interfaceOwner || method.isSynthesized)
                {
                    continue;
                }
                if (IsInterfaceMethodCandidate(method))
                {
                    RecordMissingInterfaceMethod(method, interfaceName, owners, table, missing);
                }
                else if (method.type == SymbolType::Property)
                {
                    std::vector<Symbol> expanded;
                    ExpandVirtualPropertyAccessors(method, expanded);
                    for (const auto& accessor : expanded)
                    {
                        RecordMissingInterfaceMethod(accessor, interfaceName, owners, table, missing);
                    }
                }
            }
        }
    }
}

std::vector<MissingInterfaceMethod> FindMissingInterfaceMethods(const Symbol& cls, const SymbolTable& table)
{
    if (cls.type != SymbolType::Class || cls.GetClass().modifiers.isMixin)
    {
        return {};
    }
    const auto& name = cls.qualifiedName.empty() ? cls.name : cls.qualifiedName;
    const auto hierarchy = GetInheritedTypeHierarchy(name, table);
    std::vector<std::string> owners;
    for (const auto& owner : hierarchy)
    {
        if (!IsInterfaceContract(owner, table))
        {
            owners.push_back(owner);
        }
    }

    std::vector<std::string> interfaces;
    for (const auto& base : cls.GetClass().bases)
    {
        auto qualified = CleanBaseType(QualifyTypeInContainer(base, cls.containerName, table));
        if (IsInterfaceContract(qualified, table))
        {
            interfaces.push_back(std::move(qualified));
        }
    }
    for (const auto& ancestor : hierarchy)
    {
        if (IsInterfaceContract(ancestor, table) &&
            std::find(interfaces.begin(), interfaces.end(), ancestor) == interfaces.end())
        {
            interfaces.push_back(ancestor);
        }
    }

    std::vector<MissingInterfaceMethod> missing;
    for (const auto& interfaceName : interfaces)
    {
        AppendMissingMethods(interfaceName, owners, table, missing);
    }
    return missing;
}

bool IsInterfaceMethod(const Symbol& sym, const SymbolTable& table)
{
    if (sym.containerName.empty())
    {
        return false;
    }
    const auto owners = table.FindSymbolsPtr(sym.containerName);
    if (!owners)
    {
        return false;
    }
    for (const auto& owner : *owners)
    {
        if (owner.type == SymbolType::Interface)
        {
            return true;
        }
    }
    return false;
}

size_t CountInterfaceMethodImplementations(const Symbol& sym, const SymbolTable& table)
{
    size_t implCount = 0;
    table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& candidates)
        {
            for (const auto& cand : candidates)
            {
                if (cand.type != SymbolType::Function || cand.name != sym.name ||
                    (cand.fileUri == sym.fileUri && cand.containerName == sym.containerName) ||
                    cand.containerName.empty())
                {
                    continue;
                }
                const auto candOwners = table.FindSymbolsPtr(cand.containerName);
                if (!candOwners)
                {
                    continue;
                }
                for (const auto& cOwner : *candOwners)
                {
                    if (cOwner.type == SymbolType::Class)
                    {
                        for (const auto& b : cOwner.GetClass().bases)
                        {
                            if (CleanBaseTypeView(b) == sym.containerName &&
                                cand.GetFunction().parameters.size() == sym.GetFunction().parameters.size())
                            {
                                implCount++;
                            }
                        }
                    }
                }
            }
        });
    return implCount;
}

size_t CountInterfaceImplementations(const Symbol& sym, const SymbolTable& table)
{
    size_t implCount = 0;
    table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& candidates)
        {
            for (const auto& cand : candidates)
            {
                if (cand.type == SymbolType::Class)
                {
                    for (const auto& b : cand.GetClass().bases)
                    {
                        if (CleanBaseTypeView(b) == sym.name)
                        {
                            implCount++;
                            break;
                        }
                    }
                }
            }
        });
    return implCount;
}

std::string FormatImplementationCount(size_t implCount, const i18n::I18n* i18n)
{
    return (implCount == 1) ? i18n::FormatMessage(i18n, "codelens.implementation", "1 implementation")
                            : i18n::FormatMessage(i18n, "codelens.implementations", "{} implementations", implCount);
}
} // namespace angel_lsp::analysis
