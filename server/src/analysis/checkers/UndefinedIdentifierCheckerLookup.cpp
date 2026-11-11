#include "analysis/checkers/UndefinedIdentifierCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"

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

    if (ctx.request.nodeIndex != nullptr || ctx.request.tree != nullptr)
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
