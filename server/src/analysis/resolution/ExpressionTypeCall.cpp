#include "analysis/resolution/ExpressionTypeCall.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeMember.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

void CollectMixinCallCandidates(std::string_view containerName, std::string_view funcName,
                                const SymbolTable& symbolTable, std::vector<Symbol>& candidates)
{
    if (const auto classSyms = symbolTable.FindSymbolsPtr(containerName))
    {
        for (const auto& cs : *classSyms)
        {
            if (cs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cs.signature))
            {
                for (const auto& mixin : cs.GetClass().includedMixins)
                {
                    if (const auto mixinFound = symbolTable.FindMemberSymbolPtr(mixin, funcName))
                    {
                        for (const auto& sym : *mixinFound)
                        {
                            AppendCandidateIfUnique(candidates, sym);
                        }
                    }
                }
            }
        }
    }
}

std::vector<Symbol> CollectEnclosingCallCandidates(std::span<const ContainerInfo> containers, std::string_view funcName,
                                                   const SymbolTable& symbolTable, ExpressionTypeCache* cache)
{
    std::vector<Symbol> candidates;
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            const std::string_view containerName =
                c.qualifiedName.empty() ? std::string_view(c.name) : std::string_view(c.qualifiedName);
            auto hierarchy = GetInheritedTypeHierarchy(containerName, symbolTable, cache);
            for (const auto& cls : hierarchy)
            {
                if (const auto found = symbolTable.FindMemberSymbolPtr(cls, funcName))
                {
                    for (const auto& sym : *found)
                    {
                        AppendCandidateIfUnique(candidates, sym);
                    }
                }
            }
            CollectMixinCallCandidates(containerName, funcName, symbolTable, candidates);
            break;
        }
    }
    return candidates;
}

std::string ResolveCallOverloadReturn(std::span<const Symbol> candidates, std::span<const std::string_view> argTypes,
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
        std::string ret = CleanExpressionType(chosen->GetFunction().returnType);
        if (ret.empty() &&
            (chosen->name == chosen->containerName || LastScopeSegment(chosen->containerName) == chosen->name))
        {
            return chosen->containerName;
        }
        return ret;
    }
    std::string ret = CleanExpressionType(candidates[0].GetFunction().returnType);
    if (ret.empty() && (candidates[0].name == candidates[0].containerName ||
                        LastScopeSegment(candidates[0].containerName) == candidates[0].name))
    {
        return candidates[0].containerName;
    }
    return ret;
}

std::string ResolveCallOverloadReturn(std::span<const Symbol> candidates, const std::vector<std::string>& argTypes,
                                      const SymbolTable& symbolTable)
{
    std::vector<std::string_view> views;
    views.reserve(argTypes.size());
    for (const auto& a : argTypes)
    {
        views.emplace_back(a);
    }
    return ResolveCallOverloadReturn(candidates, std::span<const std::string_view>(views), symbolTable);
}

void CollectScopeAndGlobalFunctions(std::string_view funcName, std::span<const Symbol> inScopeSymbols,
                                    const SymbolTable& symbolTable, std::vector<Symbol>& candidates)
{
    for (const auto& sym : inScopeSymbols)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            candidates.push_back(sym);
        }
    }
    if (!candidates.empty())
    {
        return;
    }
    if (const auto globalFound = symbolTable.FindSymbolsPtr(funcName))
    {
        for (const auto& sym : *globalFound)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                candidates.push_back(sym);
            }
        }
    }
}

std::string FindClassOrInterfaceName(std::string_view funcName, const SymbolTable& symbolTable)
{
    const auto templatePos = funcName.find('<');
    const std::string_view baseName =
        (templatePos != std::string_view::npos) ? funcName.substr(0, templatePos) : funcName;

    if (const auto globalFound = symbolTable.FindSymbolsPtr(baseName))
    {
        for (const auto& sym : *globalFound)
        {
            if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
            {
                if (templatePos != std::string_view::npos)
                {
                    if (!sym.containerName.empty() && !utils::ContainsScopeSeparator(funcName))
                    {
                        return utils::JoinScopedName(sym.containerName, funcName);
                    }
                    return std::string(funcName);
                }
                if (!sym.qualifiedName.empty())
                {
                    return sym.qualifiedName;
                }
                if (!sym.containerName.empty())
                {
                    return utils::JoinScopedName(sym.containerName, sym.name);
                }
                return sym.name;
            }
        }
    }
    return "";
}

std::string ResolveMemberCallReturn(const MemberCallResolutionRequest& req, std::span<const std::string_view> argTypes,
                                    const SymbolTable& symbolTable)
{
    std::string objType = CanonicalizeArrayType(req.objType, req.arrayTypeName);
    std::string ownerType = MemberOwnerType(objType, req.arrayTypeName);
    if (ownerType.empty())
    {
        ownerType = CleanExpressionType(objType);
    }
    if (ownerType.empty())
    {
        ownerType = std::string(req.fallbackObjName);
    }

    auto candidates = CollectMemberMethodCandidates(ownerType, req.memName, symbolTable);
    if (!candidates.empty())
    {
        return ResolveCandidateMethodReturn(candidates, argTypes, objType, symbolTable);
    }

    auto templateInfo = ParseTemplateType(objType);
    return ResolveContainerMethodFallback(req.memName, templateInfo, objType, req.arrayTypeName);
}

std::string ResolveMemberCallReturn(const MemberCallResolutionRequest& req, const std::vector<std::string>& argTypes,
                                    const SymbolTable& symbolTable)
{
    std::vector<std::string_view> views;
    views.reserve(argTypes.size());
    for (const auto& a : argTypes)
    {
        views.emplace_back(a);
    }
    return ResolveMemberCallReturn(req, std::span<const std::string_view>(views), symbolTable);
}

std::string ResolveCalleeFuncdefReturn(std::string_view calleeType, const SymbolTable& symbolTable)
{
    const std::string_view cleanCallee = CleanBaseTypeView(calleeType);
    if (!cleanCallee.empty())
    {
        if (auto funcdef = FindFuncdefSymbol(cleanCallee, symbolTable))
        {
            return CleanExpressionType(funcdef->GetFuncdef().returnType);
        }
    }
    return std::string(calleeType);
}

} // namespace angel_lsp::analysis
