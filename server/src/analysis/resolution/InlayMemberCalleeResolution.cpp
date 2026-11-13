#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/InlayCalleeResolutionInternal.h"
#include "analysis/resolution/InlayResolutionInternal.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Collects member symbols across an inheritance hierarchy into candidateSymbols.
 * @param[in] hierarchy Type hierarchy list.
 * @param[in] memberName Member identifier text.
 * @param[in] symbolTable Symbol table to look up in.
 * @param[in,out] candidateSymbols Output symbol vector.
 */
static void CollectHierarchyMembers(std::span<const std::string> hierarchy, std::string_view memberName,
                                    const SymbolTable& symbolTable, std::vector<Symbol>& candidateSymbols)
{
    for (const auto& typeName : hierarchy)
    {
        if (const auto found = symbolTable.FindMemberSymbolPtr(typeName, memberName))
        {
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Function)
                {
                    bool overriddenLower =
                        std::any_of(candidateSymbols.begin(), candidateSymbols.end(),
                                    [&](const Symbol& kept) { return HasSameParameterList(kept, sym); });
                    if (!overriddenLower)
                    {
                        candidateSymbols.push_back(sym);
                    }
                }
                else
                {
                    candidateSymbols.push_back(sym);
                }
            }
        }
    }
}

std::vector<Symbol> CollectMemberCalleeCandidates(const IndexedCallSite& site, const ExpressionTypeContext& ctx)
{
    std::vector<Symbol> candidateSymbols;
    if (site.receiverStartByte == 0 && site.receiverEndByte == 0)
    {
        return candidateSymbols;
    }

    std::string_view objText =
        ctx.sourceCode.substr(site.receiverStartByte, site.receiverEndByte - site.receiverStartByte);
    const std::string_view memText = site.calleeText;
    const IndexedByteRange receiverRange{site.receiverStartByte, site.receiverEndByte};
    ReceiverTypeContext recvCtx{ctx.scope, "", ctx.uri};
    recvCtx.nodeIndex = ctx.nodeIndex;
    std::string receiverTypeName = ResolveReceiverType(receiverRange, ctx.sourceCode, ctx.symbolTable, recvCtx);

    if (receiverTypeName.empty())
    {
        if (const auto directCandidates = ctx.symbolTable.FindMemberSymbolPtr(objText, memText))
        {
            candidateSymbols = *directCandidates;
        }
        return candidateSymbols;
    }

    auto hierarchy = GetInheritedTypeHierarchy(receiverTypeName, ctx.symbolTable);
    CollectHierarchyMembers(hierarchy, memText, ctx.symbolTable, candidateSymbols);
    return candidateSymbols;
}

void CollectEnclosingClassCallees(std::string_view calleeName, uint32_t callByteOffset,
                                  const ExpressionTypeContext& ctx, std::vector<Symbol>& candidateSymbols)
{
    std::vector<ContainerInfo> containers;
    if (ctx.nodeIndex)
    {
        containers =
            GetEnclosingContainers(IndexedByteRange{callByteOffset, callByteOffset}, *ctx.nodeIndex, ctx.sourceCode);
    }
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            auto hierarchy =
                GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName, ctx.symbolTable);
            CollectHierarchyMembers(hierarchy, calleeName, ctx.symbolTable, candidateSymbols);
            break;
        }
    }
}

std::vector<Symbol> CollectScopedCalleeCandidates(std::string_view calleeName, uint32_t callByteOffset,
                                                  const ExpressionTypeContext& ctx)
{
    std::vector<Symbol> candidates;
    const auto [targetContainer, methodName] = utils::SplitScopedName(calleeName);
    if (targetContainer.empty())
    {
        return candidates;
    }

    auto hierarchy = GetInheritedTypeHierarchy(targetContainer, ctx.symbolTable);
    for (const auto& typeName : hierarchy)
    {
        if (const auto syms = ctx.symbolTable.FindMemberSymbolPtr(typeName, methodName))
        {
            if (!syms->empty())
            {
                candidates.insert(candidates.end(), syms->begin(), syms->end());
                return candidates;
            }
        }
    }

    std::vector<ContainerInfo> containers;
    if (ctx.nodeIndex)
    {
        containers =
            GetEnclosingContainers(IndexedByteRange{callByteOffset, callByteOffset}, *ctx.nodeIndex, ctx.sourceCode);
    }
    for (auto it = containers.rbegin(); it != containers.rend(); ++it)
    {
        if (it->kind == ContainerKind::Namespace)
        {
            const std::string_view nsPrefix =
                it->qualifiedName.empty() ? std::string_view(it->name) : std::string_view(it->qualifiedName);
            if (const auto scopedSyms = ctx.symbolTable.FindMemberSymbolPtr(nsPrefix, calleeName))
            {
                if (!scopedSyms->empty())
                {
                    return *scopedSyms;
                }
            }
            auto scopedHier =
                GetInheritedTypeHierarchy(utils::JoinScopedName(nsPrefix, targetContainer), ctx.symbolTable);
            for (const auto& typeName : scopedHier)
            {
                if (const auto syms = ctx.symbolTable.FindMemberSymbolPtr(typeName, methodName))
                {
                    if (!syms->empty())
                    {
                        candidates.insert(candidates.end(), syms->begin(), syms->end());
                        return candidates;
                    }
                }
            }
        }
    }
    return candidates;
}

} // namespace angel_lsp::analysis
