#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

bool NamesAType(const std::string& name, const SymbolTable& table)
{
    if (IsCorePrimitive(name))
    {
        return true;
    }
    if (const auto symbols = table.FindSymbolsPtr(name))
    {
        if (std::any_of(symbols->begin(), symbols->end(),
                        [](const Symbol& sym)
                        {
                            return sym.type == SymbolType::Class || sym.type == SymbolType::Interface ||
                                   sym.type == SymbolType::Funcdef || sym.type == SymbolType::Enum ||
                                   sym.type == SymbolType::Typedef;
                        }))
        {
            return true;
        }
    }
    if (!HasScopeQualifier(name))
    {
        const std::string_view shortName = LastScopeSegment(name);
        const auto matches = table.FindTypeSymbolsByShortName(shortName);
        return !matches.empty();
    }
    return false;
}

bool IsFunctionSymbol(const Symbol& sym)
{
    return sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature);
}

CandidateSet JudgeAgainst(const std::vector<Symbol>& candidates, uint32_t argumentCount)
{
    CandidateSet result;
    for (const auto& sym : candidates)
    {
        result.decided = true;

        const auto arity = InspectFunctionArity(sym.GetFunction());
        if (IsArityCompatible(arity, argumentCount))
        {
            result.accepts = true;
            return result;
        }
    }
    return result;
}

std::vector<std::string> CollectReachableScopes(TSNode callNode, std::string_view sourceCode,
                                                const NodeIndex* nodeIndex)
{
    std::vector<std::string> reachableScopes;
    for (const auto& container : GetEnclosingContainers(callNode, sourceCode, nodeIndex))
    {
        if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            continue;
        }
        if (container.kind == ContainerKind::Namespace)
        {
            reachableScopes.push_back(container.qualifiedName);
        }
    }
    reachableScopes.emplace_back();
    return reachableScopes;
}

static void CollectScopeCandidates(const std::string& scopeName, const std::string& name, const FreeLookupContext& ctx,
                                   std::vector<Symbol>& candidates)
{
    const auto found =
        scopeName.empty() ? ctx.table.FindSymbolsPtr(name) : ctx.table.FindMemberSymbolPtr(scopeName, name);
    if (!found)
    {
        return;
    }

    for (const auto& sym : *found)
    {
        const bool isVisible = sym.fileUri == ctx.fileUri ||
                               (!ctx.moduleFileUris.empty() && ctx.moduleFileUris.contains(sym.fileUri)) ||
                               utils::IsPredefinedFile(sym.fileUri, ctx.predefinedExtension);
        if (IsFunctionSymbol(sym) && isVisible)
        {
            candidates.push_back(sym);
        }
    }
}

static void CollectUsingNamespaceCandidates(const std::string& name, const std::vector<std::string>& reachableScopes,
                                            const FreeLookupContext& ctx, std::vector<Symbol>& candidates)
{
    const auto usings = CollectVisibleUsingNamespaces(ctx.callNode, ctx.sourceCode, ctx.nodeIndex);

    for (const auto& imported : usings)
    {
        if (!imported.empty() &&
            std::find(reachableScopes.begin(), reachableScopes.end(), imported) == reachableScopes.end())
        {
            CollectScopeCandidates(imported, name, ctx, candidates);
        }
    }
}

std::vector<Symbol> FindFreeCandidates(const std::string& name, const FreeLookupContext& ctx)
{
    std::vector<Symbol> candidates;
    if (!ctx.index.allNames.contains(name))
    {
        return candidates;
    }

    const std::vector<std::string> reachableScopes =
        CollectReachableScopes(ctx.callNode, ctx.sourceCode, ctx.nodeIndex);
    for (const auto& scopeName : reachableScopes)
    {
        CollectScopeCandidates(scopeName, name, ctx, candidates);
        if (!candidates.empty())
        {
            return candidates;
        }
    }

    CollectUsingNamespaceCandidates(name, reachableScopes, ctx, candidates);
    return candidates;
}

std::vector<Symbol> CollectScopedFunctionCandidates(std::string_view written, const SymbolTable& table)
{
    std::vector<Symbol> candidates;
    if (const auto found = table.FindSymbolsPtr(written))
    {
        for (const auto& sym : *found)
        {
            if (IsFunctionSymbol(sym))
            {
                candidates.push_back(sym);
            }
        }
    }
    return candidates;
}

} // namespace angel_lsp::analysis
