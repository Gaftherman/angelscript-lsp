#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/DefinitionResolution.h"
#include "analysis/resolution/DefinitionResolutionInternal.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/types/VirtualMixinContext.h"
#include "config/EngineRuleConfig.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Determines whether a candidate parent symbol's type corresponds to the declaring container kind recorded on a
 * candidate member symbol during scope disambiguation.
 * @param[in] containerType Symbol type of the candidate enclosing parent symbol.
 * @param[in] memberContainerKind Container kind classification recorded on the resolved member symbol.
 * @return True if the candidate parent's symbol type corresponds to the member's container kind (Class, Interface, or
 * Namespace); false for non-container types or mismatched pairings.
 */
static bool ContainerKindMatches(SymbolType containerType, ContainerKind memberContainerKind) noexcept
{
    switch (containerType)
    {
    case SymbolType::Class:
        return memberContainerKind == ContainerKind::Class;
    case SymbolType::Interface:
        return memberContainerKind == ContainerKind::Interface;
    case SymbolType::Namespace:
        return memberContainerKind == ContainerKind::Namespace;
    default:
        return false;
    }
}

void DisambiguateCandidateContainers(std::vector<Symbol>& symbols, const Scope* innerScope,
                                     const DefinitionQuery& query)
{
    if (symbols.size() <= 1)
    {
        return;
    }
    std::string_view memberName;
    const auto* ref = FindReferenceAt(innerScope, query.line, query.character);
    if (ref && ref->isScopedIdentifier && ref->qualifierStartByte < ref->qualifierEndByte &&
        ref->qualifierEndByte <= query.sourceCode.size())
    {
        const std::string_view scopedText =
            query.sourceCode.substr(ref->qualifierStartByte, ref->qualifierEndByte - ref->qualifierStartByte);
        memberName = utils::SplitScopedName(scopedText).second;
    }
    if (memberName.empty() && !query.scopedIdentifier.empty() && utils::ContainsScopeSeparator(query.scopedIdentifier))
    {
        memberName = utils::SplitScopedName(query.scopedIdentifier).second;
    }
    if (memberName.empty())
    {
        return;
    }
    std::vector<Symbol> matched;
    for (const auto& sym : symbols)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface || sym.type == SymbolType::Namespace)
        {
            const std::string_view containerName =
                sym.qualifiedName.empty() ? std::string_view(sym.name) : std::string_view(sym.qualifiedName);
            if (const auto memberSyms = query.symbolTable.FindMemberSymbolPtr(containerName, memberName))
            {
                for (const auto& m : *memberSyms)
                {
                    if (ContainerKindMatches(sym.type, m.containerKind))
                    {
                        matched.push_back(sym);
                        break;
                    }
                }
            }
        }
    }
    if (matched.size() == 1)
    {
        symbols = std::move(matched);
    }
}

std::vector<Symbol> ResolveClassMemberSymbols(const TargetDescriptor& target, const Scope* innerScope,
                                              const DefinitionQuery& query)
{
    auto memberSyms = query.symbolTable.FindMemberSymbols(target.declaringClass, target.name);
    const auto* ref = FindReferenceAt(innerScope, query.line, query.character);
    const size_t argCount = (ref && ref->isCall) ? static_cast<size_t>(ref->argumentCount) : 0;
    if (argCount > 0)
    {
        std::vector<Symbol> arityMatched;
        for (const auto& s : memberSyms)
        {
            if (s.type == SymbolType::Function && s.GetFunction().parameters.size() == argCount)
            {
                arityMatched.push_back(s);
            }
        }
        if (!arityMatched.empty())
        {
            return arityMatched;
        }
        for (const auto& cls : target.relatedClasses)
        {
            if (const auto relSyms = query.symbolTable.FindMemberSymbolPtr(cls, target.name))
            {
                for (const auto& s : *relSyms)
                {
                    if (s.type == SymbolType::Function && s.GetFunction().parameters.size() == argCount)
                    {
                        arityMatched.push_back(s);
                    }
                }
                if (!arityMatched.empty())
                {
                    return arityMatched;
                }
            }
        }
    }
    if (memberSyms.empty())
    {
        for (const auto& cls : target.relatedClasses)
        {
            if (const auto relSyms = query.symbolTable.FindMemberSymbolPtr(cls, target.name))
            {
                if (!relSyms->empty())
                {
                    memberSyms = *relSyms;
                    break;
                }
            }
        }
    }
    return memberSyms;
}

std::vector<Symbol> TryResolveScopedIdentifier(const DefinitionQuery& query)
{
    std::string_view scopedName = query.scopedIdentifier;
    if (scopedName.empty() || !utils::ContainsScopeSeparator(scopedName))
    {
        return {};
    }

    auto [qualifier, memberName] = utils::SplitScopedName(scopedName);
    if (qualifier.empty() || memberName.empty())
    {
        return {};
    }

    auto memberSyms = query.symbolTable.FindMemberSymbols(qualifier, memberName);
    if (memberSyms.empty())
    {
        memberSyms = query.symbolTable.FindSymbols(scopedName);
    }
    if (memberSyms.empty())
    {
        const std::string enclosingNs = FindEnclosingNamespace(query.uri, query.line, query.symbolTable);
        if (!enclosingNs.empty())
        {
            utils::ForEachEnclosingScope(
                enclosingNs,
                [&](std::string_view scope)
                {
                    if (!memberSyms.empty())
                    {
                        return;
                    }
                    if (query.symbolTable.HasScopedSymbol(scope, qualifier))
                    {
                        if (const auto containers = query.symbolTable.FindMemberSymbolPtr(scope, qualifier))
                        {
                            for (const auto& cont : *containers)
                            {
                                const std::string_view contName = cont.qualifiedName.empty()
                                                                      ? std::string_view(cont.name)
                                                                      : std::string_view(cont.qualifiedName);
                                if (const auto found = query.symbolTable.FindMemberSymbolPtr(contName, memberName))
                                {
                                    if (!found->empty())
                                    {
                                        memberSyms = *found;
                                        return;
                                    }
                                }
                            }
                        }
                    }
                    if (const auto scoped = query.symbolTable.FindMemberSymbolPtr(scope, scopedName))
                    {
                        memberSyms = *scoped;
                    }
                });
        }
    }
    return memberSyms;
}

std::vector<Symbol> TryResolveThisKeyword(std::string_view nodeText, const DefinitionQuery& query)
{
    if (nodeText != "this")
    {
        return {};
    }
    std::string enclosingClass = query.symbolTable.FindEnclosingClassName(query.uri, query.line);
    if (enclosingClass.empty())
    {
        return {};
    }
    return query.symbolTable.FindSymbols(enclosingClass);
}

std::vector<Symbol> TryResolveDelegateProperty(std::string_view nodeText, const DefinitionQuery& query)
{
    if (!IsBaseClassDelegateProperty(nodeText, query.symbolTable, query.engineRules))
    {
        return {};
    }
    std::string enclosingClass = query.symbolTable.FindEnclosingClassName(query.uri, query.line);
    if (enclosingClass.empty())
    {
        return {};
    }
    std::string baseClass = ResolveBaseClass(enclosingClass, query.symbolTable);
    if (!baseClass.empty())
    {
        return query.symbolTable.FindSymbols(baseClass);
    }
    return {};
}

std::vector<Symbol> TryResolveVirtualMixin(std::string_view nodeText, const DefinitionQuery& query)
{
    auto vCtx = ResolveVirtualMixinContext(query.uri, query.symbolTable);
    if (!vCtx.isVirtual)
    {
        return {};
    }
    if (const auto found = query.symbolTable.FindMemberSymbolPtr(vCtx.hostClass, nodeText))
    {
        if (!found->empty())
        {
            return *found;
        }
    }
    auto hierarchy = GetInheritedTypeHierarchy(vCtx.hostClass, query.symbolTable);
    for (const auto& base : hierarchy)
    {
        if (const auto found = query.symbolTable.FindMemberSymbolPtr(base, nodeText))
        {
            if (!found->empty())
            {
                return *found;
            }
        }
    }
    if (nodeText == "this" || IsBaseClassDelegateProperty(nodeText, query.symbolTable, query.engineRules))
    {
        return query.symbolTable.FindSymbols(vCtx.hostClass);
    }
    return {};
}

} // namespace angel_lsp::analysis
