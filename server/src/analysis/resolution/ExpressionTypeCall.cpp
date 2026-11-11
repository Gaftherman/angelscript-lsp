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

void CollectMixinCallCandidates(std::string_view containerName, const std::string& funcName,
                                const SymbolTable& symbolTable, std::vector<Symbol>& candidates)
{
    auto classSyms = symbolTable.FindSymbols(containerName);
    for (const auto& cs : classSyms)
    {
        if (cs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cs.signature))
        {
            for (const auto& mixin : cs.GetClass().includedMixins)
            {
                auto mixinFound = symbolTable.FindMemberSymbols(mixin, funcName);
                for (const auto& sym : mixinFound)
                {
                    AppendCandidateIfUnique(candidates, sym);
                }
            }
        }
    }
}

std::vector<Symbol> CollectEnclosingCallCandidates(const std::vector<ContainerInfo>& containers,
                                                   const std::string& funcName,
                                                   const SymbolTable& symbolTable,
                                                   ExpressionTypeCache* cache)
{
    std::vector<Symbol> candidates;
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            std::string containerName = c.qualifiedName.empty() ? c.name : c.qualifiedName;
            auto hierarchy = GetInheritedTypeHierarchy(containerName, symbolTable, cache);
            for (const auto& cls : hierarchy)
            {
                auto found = symbolTable.FindMemberSymbols(cls, funcName);
                for (const auto& sym : found)
                {
                    AppendCandidateIfUnique(candidates, sym);
                }
            }
            CollectMixinCallCandidates(containerName, funcName, symbolTable, candidates);
            break;
        }
    }
    return candidates;
}

std::string ResolveCallOverloadReturn(const std::vector<Symbol>& candidates,
                                      const std::vector<std::string>& argTypes,
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

void CollectScopeAndGlobalFunctions(const std::string& funcName, const std::vector<Symbol>& inScopeSymbols,
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
    auto globalFound = symbolTable.FindSymbols(funcName);
    for (const auto& sym : globalFound)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            candidates.push_back(sym);
        }
    }
}

std::string FindClassOrInterfaceName(const std::string& funcName, const SymbolTable& symbolTable)
{
    const auto templatePos = funcName.find('<');
    const std::string_view baseName = (templatePos != std::string::npos)
                                          ? std::string_view(funcName).substr(0, templatePos)
                                          : std::string_view(funcName);

    auto globalFound = symbolTable.FindSymbols(baseName);
    for (const auto& sym : globalFound)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
        {
            if (templatePos != std::string::npos)
            {
                if (!sym.containerName.empty() && !utils::ContainsScopeSeparator(funcName))
                {
                    return utils::JoinScopedName(sym.containerName, funcName);
                }
                return funcName;
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
    return "";
}

std::string ResolveMemberCallReturn(const MemberCallResolutionRequest& req,
                                    const std::vector<std::string>& argTypes,
                                    const SymbolTable& symbolTable)
{
    std::string objType = CanonicalizeArrayType(std::string(req.objType), req.arrayTypeName);
    std::string ownerType = MemberOwnerType(objType, req.arrayTypeName);
    if (ownerType.empty())
    {
        ownerType = CleanExpressionType(objType);
    }
    if (ownerType.empty())
    {
        ownerType = std::string(req.fallbackObjName);
    }

    std::string memName(req.memName);
    auto candidates = CollectMemberMethodCandidates(ownerType, memName, symbolTable);
    if (!candidates.empty())
    {
        return ResolveCandidateMethodReturn(candidates, argTypes, objType, symbolTable);
    }

    auto templateInfo = ParseTemplateType(objType);
    return ResolveContainerMethodFallback(req.memName, templateInfo, objType, req.arrayTypeName);
}

std::string ResolveCalleeFuncdefReturn(std::string_view calleeType, const SymbolTable& symbolTable)
{
    std::string cleanCallee = CleanBaseType(calleeType);
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
