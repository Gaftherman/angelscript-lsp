#include "analysis/resolution/ExpressionTypeMember.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

std::vector<std::string> OrderMemberPropertySearchHierarchy(std::string_view cleanObj, const SymbolTable& symbolTable)
{
    auto hierarchy = GetInheritedTypeHierarchy(cleanObj, symbolTable);
    if (hierarchy.empty() && !cleanObj.empty())
    {
        hierarchy.emplace_back(cleanObj);
    }
    std::vector<std::string> propSearchOrder;
    if (!hierarchy.empty())
    {
        propSearchOrder.push_back(hierarchy[0]);
        for (size_t i = 1; i < hierarchy.size(); ++i)
        {
            if (!IsMixinClass(hierarchy[i], symbolTable))
            {
                propSearchOrder.push_back(hierarchy[i]);
            }
        }
        for (size_t i = 1; i < hierarchy.size(); ++i)
        {
            if (IsMixinClass(hierarchy[i], symbolTable))
            {
                propSearchOrder.push_back(hierarchy[i]);
            }
        }
    }
    return propSearchOrder;
}

static bool IsNonEnumSymbol(const Symbol& s)
{
    if (s.type == SymbolType::Variable && std::holds_alternative<VariableSignature>(s.signature))
    {
        return !s.GetVariable().isEnumConstant;
    }
    return true;
}

std::vector<Symbol> FindMemberSymbolsInType(std::string_view typeName, std::string_view memName,
                                            const SymbolTable& symbolTable)
{
    auto found = symbolTable.FindMemberSymbols(typeName, memName);
    const bool hasInstance = std::any_of(found.begin(), found.end(), IsNonEnumSymbol);
    if (!hasInstance)
    {
        std::string getProp;
        getProp.reserve(4 + memName.size());
        getProp.append("get_");
        getProp.append(memName);

        std::string setProp;
        setProp.reserve(4 + memName.size());
        setProp.append("set_");
        setProp.append(memName);

        auto getters = symbolTable.FindMemberSymbols(typeName, getProp);
        auto setters = symbolTable.FindMemberSymbols(typeName, setProp);
        if (!getters.empty() || !setters.empty())
        {
            found.insert(found.begin(), setters.begin(), setters.end());
            found.insert(found.begin(), getters.begin(), getters.end());
        }
    }
    if (found.empty())
    {
        auto allSymbols = symbolTable.FindSymbols(memName);
        for (const auto& sSym : allSymbols)
        {
            if (sSym.containerName == typeName)
            {
                found.push_back(sSym);
            }
        }
    }
    std::stable_partition(found.begin(), found.end(), IsNonEnumSymbol);
    return found;
}

std::string ExtractAccessorMemberType(const Symbol& sym)
{
    if (!std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return "";
    }
    const auto& fn = sym.GetFunction();
    const bool isGet = sym.name.starts_with("get_");
    const bool isSet = sym.name.starts_with("set_");
    if (!isGet && !isSet && !fn.modifiers.isProperty)
    {
        return "";
    }
    const bool isIndexed = (isGet && !fn.parameters.empty()) || (isSet && fn.parameters.size() > 1);
    if (isIndexed)
    {
        return "";
    }
    if (!fn.returnType.empty() && fn.returnType != "void")
    {
        return CleanExpressionType(fn.returnType);
    }
    if (!fn.parameters.empty())
    {
        return CleanExpressionType(fn.parameters.back().typeName);
    }
    return "";
}

std::string ExtractMemberSymbolType(const Symbol& sym, const SymbolTable& table)
{
    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) && !sym.GetVariable().typeName.empty())
    {
        std::string type = sym.GetVariable().typeName;
        if (!sym.containerName.empty())
        {
            type = QualifyTypeInContainer(type, sym.containerName, table);
        }
        return CleanExpressionType(type);
    }
    if (sym.type == SymbolType::Function)
    {
        std::string type = ExtractAccessorMemberType(sym);
        if (!sym.containerName.empty())
        {
            type = QualifyTypeInContainer(type, sym.containerName, table);
        }
        return type;
    }
    return "";
}

std::string ResolveContainerMethodFallback(std::string_view memName, const TemplateTypeInfo& templateInfo,
                                           std::string_view objType, std::string_view arrayTypeName)
{
    const std::string_view effectiveArrayName = arrayTypeName;
    if ((templateInfo.containerName == effectiveArrayName && !templateInfo.templateArgs.empty()) ||
        objType.ends_with("[]"))
    {
        if (memName == "length" || memName == "size")
        {
            return "uint";
        }
        if (memName == "isEmpty")
        {
            return "bool";
        }
    }
    return "";
}

std::string ResolveCandidateMethodReturn(std::span<const Symbol> candidates,
                                         std::span<const std::string_view> argTypes, std::string_view objType,
                                         const SymbolTable& symbolTable)
{
    if (candidates.empty())
    {
        return "";
    }
    auto match = ResolveBestOverload(candidates, argTypes, symbolTable);
    const Symbol* chosen = match.bestCandidate ? match.bestCandidate : &candidates[0];
    if (chosen && std::holds_alternative<FunctionSignature>(chosen->signature))
    {
        std::string ret = chosen->GetFunction().returnType;
        auto binding = BindTemplateArguments(objType, symbolTable);
        if (binding.usable)
        {
            ret = SubstituteTemplateParameters(ret, binding);
        }
        else
        {
            auto templateInfo = ParseTemplateType(objType);
            if (!templateInfo.templateArgs.empty())
            {
                ret = SubstituteTypeParam(ret, "T", templateInfo.templateArgs[0]);
            }
        }
        return CleanExpressionType(ret);
    }
    return CleanExpressionType(candidates[0].GetFunction().returnType);
}

std::string ResolveCandidateMethodReturn(std::span<const Symbol> candidates,
                                         const std::vector<std::string>& argTypes, std::string_view objType,
                                         const SymbolTable& symbolTable)
{
    std::vector<std::string_view> views;
    views.reserve(argTypes.size());
    for (const auto& a : argTypes)
    {
        views.emplace_back(a);
    }
    return ResolveCandidateMethodReturn(candidates, std::span<const std::string_view>(views), objType, symbolTable);
}

void AppendCandidateIfUnique(std::vector<Symbol>& candidates, const Symbol& sym)
{
    if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
    {
        bool overridden = std::any_of(candidates.begin(), candidates.end(),
                                      [&](const Symbol& kept) { return HasSameParameterList(kept, sym); });
        if (!overridden)
        {
            candidates.push_back(sym);
        }
    }
}

std::vector<Symbol> CollectMemberMethodCandidates(std::string_view ownerType, std::string_view memName,
                                                  const SymbolTable& symbolTable)
{
    std::vector<Symbol> candidates;
    auto hierarchy = GetInheritedTypeHierarchy(ownerType, symbolTable);
    if (hierarchy.empty() && !ownerType.empty())
    {
        hierarchy.emplace_back(ownerType);
    }
    for (const auto& typeName : hierarchy)
    {
        auto found = symbolTable.FindMemberSymbols(typeName, memName);
        for (const auto& sym : found)
        {
            AppendCandidateIfUnique(candidates, sym);
        }
    }
    return candidates;
}

std::string ResolveMemberPropertyType(std::string_view objType, std::string_view fallbackObjName,
                                      std::string_view memName, const SymbolTable& symbolTable)
{
    if (ExpressionTypeCache::IsPoisonType(objType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    std::string_view cleanObj = CleanBaseTypeView(objType);
    if (cleanObj.empty())
    {
        cleanObj = fallbackObjName;
    }
    if (cleanObj.empty())
    {
        return "";
    }

    auto propSearchOrder = OrderMemberPropertySearchHierarchy(cleanObj, symbolTable);
    for (const auto& typeName : propSearchOrder)
    {
        auto found = FindMemberSymbolsInType(typeName, memName, symbolTable);
        for (const auto& sym : found)
        {
            if (auto resolved = ExtractMemberSymbolType(sym, symbolTable); !resolved.empty())
            {
                return resolved;
            }
        }
    }
    auto accessors = FindPropertyAccessors(cleanObj, memName, symbolTable, false);
    if (!accessors.empty())
    {
        std::string pType = PropertyTypeFromAccessors(accessors);
        if (!pType.empty())
        {
            return pType;
        }
    }
    return "";
}

} // namespace angel_lsp::analysis
