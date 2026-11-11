#include "analysis/resolution/InlayCalleeResolutionInternal.h"

#include "analysis/index/ScopeTree.h"
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
static void CollectHierarchyMembers(const std::vector<std::string>& hierarchy, const std::string& memberName,
                                    const SymbolTable& symbolTable, std::vector<Symbol>& candidateSymbols)
{
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedName = utils::JoinScopedName(typeName, memberName);
        auto found = symbolTable.FindSymbols(qualifiedName);
        for (const auto& sym : found)
        {
            if (sym.type == SymbolType::Function)
            {
                bool overriddenLower = std::any_of(candidateSymbols.begin(), candidateSymbols.end(),
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

std::vector<Symbol> CollectMemberCalleeCandidates(const IndexedCallSite& site, const ExpressionTypeContext& ctx)
{
    std::vector<Symbol> candidateSymbols;
    if (site.receiverStartByte == 0 && site.receiverEndByte == 0)
    {
        return candidateSymbols;
    }

    std::string_view objText =
        ctx.sourceCode.substr(site.receiverStartByte, site.receiverEndByte - site.receiverStartByte);
    const std::string memText(site.calleeText);
    const IndexedByteRange receiverRange{site.receiverStartByte, site.receiverEndByte};
    ReceiverTypeContext recvCtx{ctx.scope, "", ctx.uri};
    recvCtx.nodeIndex = ctx.nodeIndex;
    std::string receiverTypeName = ResolveReceiverType(receiverRange, ctx.sourceCode, ctx.symbolTable, recvCtx);

    if (receiverTypeName.empty())
    {
        auto directCandidates = ctx.symbolTable.FindMemberSymbols(std::string(objText), memText);
        for (const auto& sym : directCandidates)
        {
            candidateSymbols.push_back(sym);
        }
        return candidateSymbols;
    }

    auto hierarchy = GetInheritedTypeHierarchy(receiverTypeName, ctx.symbolTable);
    CollectHierarchyMembers(hierarchy, memText, ctx.symbolTable, candidateSymbols);
    return candidateSymbols;
}

void CollectEnclosingClassCallees(const std::string& calleeName, uint32_t callByteOffset,
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

std::vector<Symbol> CollectScopedCalleeCandidates(const std::string& calleeName, uint32_t callByteOffset,
                                                  const ExpressionTypeContext& ctx)
{
    std::vector<Symbol> candidates;
    const auto [targetContainerView, methodNameView] = utils::SplitScopedName(calleeName);
    if (targetContainerView.empty())
    {
        return candidates;
    }
    std::string targetContainer(targetContainerView);
    std::string methodName(methodNameView);

    auto hierarchy = GetInheritedTypeHierarchy(targetContainer, ctx.symbolTable);
    for (const auto& typeName : hierarchy)
    {
        auto syms = ctx.symbolTable.FindMemberSymbols(typeName, methodName);
        if (!syms.empty())
        {
            candidates.insert(candidates.end(), syms.begin(), syms.end());
            return candidates;
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
            std::string nsPrefix = it->qualifiedName.empty() ? it->name : it->qualifiedName;
            auto scopedSyms = ctx.symbolTable.FindMemberSymbols(nsPrefix, calleeName);
            if (!scopedSyms.empty())
            {
                return scopedSyms;
            }
            auto scopedHier =
                GetInheritedTypeHierarchy(utils::JoinScopedName(nsPrefix, targetContainer), ctx.symbolTable);
            for (const auto& typeName : scopedHier)
            {
                auto syms = ctx.symbolTable.FindMemberSymbols(typeName, methodName);
                if (!syms.empty())
                {
                    candidates.insert(candidates.end(), syms.begin(), syms.end());
                    return candidates;
                }
            }
        }
    }
    return candidates;
}

} // namespace angel_lsp::analysis
