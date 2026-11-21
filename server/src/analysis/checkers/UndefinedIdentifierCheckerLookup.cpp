#include "analysis/checkers/UndefinedIdentifierCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/mediator/SemanticMediator.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

MixinRanges CollectMixinRanges(const SymbolTable& symbolTable, std::string_view fileUri)
{
    MixinRanges mixinRanges;
    symbolTable.ForEachSymbolInFile(
        fileUri,
        [&]([[maybe_unused]] std::string_view qualifiedName, const std::vector<Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin)
                {
                    mixinRanges.push_back({sym.startLine, sym.endLine});
                }
            }
        });
    return mixinRanges;
}

static bool IsContainerAccessorProperty(uint32_t byteOffset, std::string_view refName, const DiagnosticContext& ctx)
{
    const auto& index = ctx.request.GetRuleIndex();
    const bool reqKeyword = ctx.request.RequiresAccessorKeyword();
    auto containers = ctx.request.nodeIndex
                          ? ctx.request.nodeIndex->CollectEnclosingContainers(byteOffset, ctx.request.sourceCode)
                          : std::vector<ContainerInfo>{};
    for (const auto& container : containers)
    {
        if (container.kind == ContainerKind::Namespace)
        {
            const std::string_view nsName = container.qualifiedName.empty() ? std::string_view(container.name)
                                                                            : std::string_view(container.qualifiedName);
            const auto& members = index.NamespaceMembers(nsName);
            const auto& accSet = reqKeyword ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
            if (accSet.contains(refName))
            {
                return true;
            }
            continue;
        }
        if (container.kind != ContainerKind::Class && container.kind != ContainerKind::Interface)
        {
            continue;
        }
        const auto hierarchy = GetInheritedTypeHierarchy(
            container.qualifiedName.empty() ? container.name : container.qualifiedName, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            const auto& members = index.ClassMembers(typeName);
            const auto& accSet = reqKeyword ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
            if (accSet.contains(refName))
            {
                return true;
            }
        }
    }
    return false;
}

bool IsEnclosingScopeMember(uint32_t byteOffset, std::string_view refName, std::span<const std::string> usingNamespaces,
                            const DiagnosticContext& ctx)
{
    const auto& index = ctx.request.GetRuleIndex();
    const auto containers = ctx.request.nodeIndex
                                ? ctx.request.nodeIndex->CollectEnclosingContainers(byteOffset, ctx.request.sourceCode)
                                : std::vector<ContainerInfo>{};
    for (const auto& container : containers)
    {
        if (container.kind == ContainerKind::Namespace)
        {
            const std::string_view nsName = container.qualifiedName.empty() ? std::string_view(container.name)
                                                                            : std::string_view(container.qualifiedName);
            if (index.NamespaceMembers(nsName).allMemberNames.contains(refName))
            {
                return true;
            }
            continue;
        }
        if (container.kind != ContainerKind::Class && container.kind != ContainerKind::Interface)
        {
            continue;
        }
        const auto hierarchy = GetInheritedTypeHierarchy(
            container.qualifiedName.empty() ? container.name : container.qualifiedName, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            if (index.ClassMembers(typeName).allMemberNames.contains(refName))
            {
                return true;
            }
        }
    }

    std::vector<std::string> localUsingsStorage;
    std::span<const std::string> activeUsings = usingNamespaces;
    if (ctx.request.nodeIndex)
    {
        localUsingsStorage = ctx.request.nodeIndex->CollectUsingNamespaces(ctx.request.sourceCode, byteOffset);
        activeUsings = localUsingsStorage;
    }
    for (const auto& uNs : activeUsings)
    {
        if (index.NamespaceMembers(uNs).allMemberNames.contains(refName))
        {
            return true;
        }
    }

    return false;
}

bool IsAccessibleInstanceProperty(std::string_view className, std::string_view name, const DiagnosticContext& ctx)
{
    const auto& request = ctx.request;
    const SemanticMediator mediator(request.symbolTable, request.scopeRoot.get(), request.nodeIndex,
                                    request.sourceCode);
    const auto& table = mediator.GetSymbolTable();
    const auto visible = [&](const Symbol& symbol, AccessModifier access)
    {
        return access != AccessModifier::Private ||
               utils::AreEquivalentGlobalOrScopedNames(className, symbol.containerName) ||
               request.TreatsPrivateAsProtected() || IsMixinClass(symbol.containerName, table);
    };
    const auto& index = request.GetRuleIndex();
    bool hasAccessor = false;
    for (const auto& owner : GetInheritedTypeHierarchy(className, table))
    {
        const auto& members = index.ClassMembers(owner);
        const auto& accessors =
            request.RequiresAccessorKeyword() ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
        hasAccessor = hasAccessor || accessors.contains(name);
        const auto symbols = table.FindMemberSymbolPtr(owner, name);
        if (!symbols)
        {
            continue;
        }
        for (const auto& symbol : *symbols)
        {
            if ((symbol.containerKind != ContainerKind::Class && symbol.containerKind != ContainerKind::Interface) ||
                (symbol.type != SymbolType::Variable && symbol.type != SymbolType::Property) ||
                !std::holds_alternative<VariableSignature>(symbol.signature) || symbol.GetVariable().isEnumConstant)
            {
                continue;
            }
            return visible(symbol, symbol.GetVariable().modifiers.access);
        }
    }
    if (!hasAccessor || !request.ScriptAccessorsAreProperties())
    {
        return false;
    }
    const auto accessors = FindPropertyAccessors(className, name, table, request.RequiresAccessorKeyword());
    return std::ranges::any_of(accessors,
                               [&](const Symbol& symbol)
                               {
                                   return (symbol.containerKind == ContainerKind::Class ||
                                           symbol.containerKind == ContainerKind::Interface) &&
                                          visible(symbol, symbol.GetFunction().modifiers.access);
                               });
}

static bool HasEnumConstant(const std::vector<Symbol>* symbols)
{
    if (symbols == nullptr)
    {
        return false;
    }
    return std::any_of(symbols->begin(), symbols->end(), [](const Symbol& sym)
                       { return sym.type == SymbolType::Variable && sym.GetVariable().isEnumConstant; });
}

bool IsEnumValueReachable(uint32_t byteOffset, std::string_view valueName, std::span<const std::string> usingNamespaces,
                          const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (HasEnumConstant(table.FindSymbolsPtr(valueName).get()))
    {
        return true;
    }
    if (!ctx.request.nodeIndex)
    {
        return true;
    }

    const auto reachableIn = [&](std::string_view container)
    { return HasEnumConstant(table.FindMemberSymbolPtr(container, valueName).get()); };

    for (const auto& container : ctx.request.nodeIndex->CollectEnclosingContainers(byteOffset, ctx.request.sourceCode))
    {
        const std::string_view path = container.qualifiedName.empty() ? std::string_view(container.name)
                                                                      : std::string_view(container.qualifiedName);
        if (container.kind == ContainerKind::Namespace)
        {
            bool found = false;
            utils::ForEachEnclosingScope(path,
                                         [&](std::string_view scope)
                                         {
                                             found = reachableIn(scope);
                                             return found;
                                         });
            if (found)
            {
                return true;
            }
        }
        else if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            for (const auto& typeName : GetInheritedTypeHierarchy(path, table))
            {
                if (reachableIn(typeName))
                {
                    return true;
                }
            }
        }
    }

    const std::vector<std::string> activeUsings =
        ctx.request.nodeIndex->CollectUsingNamespaces(ctx.request.sourceCode, byteOffset);
    const auto isImported = [&](std::string_view ns) { return reachableIn(ns); };
    return std::any_of(activeUsings.begin(), activeUsings.end(), isImported) ||
           std::any_of(usingNamespaces.begin(), usingNamespaces.end(), isImported);
}

bool IsAccessorPropertyOrKeyword(const LocalReference& ref, const DiagnosticContext& ctx)
{
    if (IsReservedKeyword(ref.name))
    {
        return true;
    }

    const auto& index = ctx.request.GetRuleIndex();
    const auto& globalAccessors = ctx.request.RequiresAccessorKeyword() ? index.keywordGlobalAccessorPropertyNames
                                                                        : index.globalAccessorPropertyNames;
    if (globalAccessors.contains(ref.name))
    {
        return true;
    }

    if (ctx.request.nodeIndex != nullptr)
    {
        if (IsContainerAccessorProperty(ref.startByte, ref.name, ctx))
        {
            return true;
        }
    }

    if (!FindGlobalPropertyAccessors(ref.name, ctx.request.symbolTable, ctx.request.RequiresAccessorKeyword()).empty())
    {
        return true;
    }

    return false;
}

} // namespace angel_lsp::analysis
