#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/InlayResolutionInternal.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
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

std::vector<Symbol> CollectMemberCalleeCandidates(TSNode funcNode, const ExpressionTypeContext& ctx)
{
    std::vector<Symbol> candidateSymbols;
    TSNode objNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return candidateSymbols;
    }

    std::string objText(parser::NodeText(objNode, ctx.sourceCode));
    std::string memText(parser::NodeText(memNode, ctx.sourceCode));
    const Scope* rootScope = ctx.scope;
    while (rootScope && rootScope->parent)
    {
        rootScope = rootScope->parent;
    }
    TSPoint objPoint = ts_node_start_point(objNode);
    const Scope* scope = rootScope ? FindInnermostScope(rootScope, objPoint.row, objPoint.column) : nullptr;
    std::string receiverTypeName = ResolveReceiverType(objNode, ctx.sourceCode, ctx.symbolTable, {scope, "", ctx.uri});

    if (receiverTypeName.empty())
    {
        auto directCandidates = ctx.symbolTable.FindMemberSymbols(objText, memText);
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

void CollectEnclosingClassCallees(const std::string& calleeName, TSNode callNode, const ExpressionTypeContext& ctx,
                                  std::vector<Symbol>& candidateSymbols)
{
    auto containers = GetEnclosingContainers(callNode, ctx.sourceCode);
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

std::vector<Symbol> CollectScopedCalleeCandidates(const std::string& calleeName, TSNode callNode,
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

    auto containers = GetEnclosingContainers(callNode, ctx.sourceCode);
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
