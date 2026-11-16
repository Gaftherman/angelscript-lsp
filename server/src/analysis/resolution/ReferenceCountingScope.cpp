#include "analysis/resolution/ReferenceCountingScope.h"

#include "analysis/resolution/ReferenceReceiver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/ScopedNameUtils.h"

#include <vector>

namespace angel_lsp::analysis
{
std::string_view GetEnclosingClassName(const SymbolTable& symbolTable, std::string_view uri, uint32_t line,
                                       EnclosingClassCache& cache)
{
    auto it = cache.find(uri);
    if (it == cache.end())
    {
        std::vector<EnclosingClassSpan> spans;
        symbolTable.ForEachSymbolInFile(
            uri,
            [&]([[maybe_unused]] std::string_view qualifiedName, const std::vector<Symbol>& syms)
            {
                for (const auto& s : syms)
                {
                    if ((s.type == SymbolType::Class || s.type == SymbolType::Interface) && s.fileUri == uri)
                    {
                        EnclosingClassSpan span;
                        span.startLine = s.startLine;
                        span.endLine = s.endLine;
                        span.name = s.qualifiedName.empty() ? s.name : s.qualifiedName;
                        spans.push_back(std::move(span));
                    }
                }
            });
        it = cache.try_emplace(uri, std::move(spans)).first;
    }
    uint32_t bestSpan = UINT32_MAX;
    std::string_view bestClass;
    for (const auto& cs : it->second)
    {
        if (line >= cs.startLine && line <= cs.endLine)
        {
            uint32_t span = cs.endLine - cs.startLine;
            if (span <= bestSpan)
            {
                bestSpan = span;
                bestClass = cs.name;
            }
        }
    }
    return bestClass;
}

bool IsContainerClass(std::string_view container, const SymbolTable& symbolTable)
{
    auto syms = symbolTable.FindSymbolsPtr(container);
    if (!syms)
    {
        return false;
    }
    for (const auto& s : *syms)
    {
        if (s.type == SymbolType::Class || s.type == SymbolType::Interface)
        {
            return true;
        }
    }
    return false;
}

bool IsSymbolInClassContainer(const Symbol& sym, const SymbolTable& symbolTable)
{
    if (sym.containerKind == ContainerKind::Class || sym.containerKind == ContainerKind::Interface)
    {
        return true;
    }
    if (sym.containerKind == ContainerKind::Namespace || sym.containerKind == ContainerKind::Enum ||
        sym.containerName.empty())
    {
        return false;
    }
    return IsContainerClass(sym.containerName, symbolTable);
}

bool IsInsideNamespace(std::string_view fileUri, uint32_t line, std::string_view nsName, const SymbolTable& symbolTable,
                       EnclosingClassCache& classCache)
{
    if (auto syms = symbolTable.FindSymbolsPtr(nsName))
    {
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Namespace && s.fileUri == fileUri)
            {
                if (line >= s.startLine && line <= s.endLine)
                {
                    return true;
                }
            }
        }
    }
    const std::string_view encClass = GetEnclosingClassName(symbolTable, fileUri, line, classCache);
    if (!encClass.empty())
    {
        if (IsSameOrNestedNamespace(encClass, nsName))
        {
            return true;
        }
        if (auto clsSyms = symbolTable.FindSymbolsPtr(encClass))
        {
            for (const auto& cs : *clsSyms)
            {
                if (IsSameOrNestedNamespace(cs.containerName, nsName))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool IsUsingNamespaceActive(const Scope* scope, uint32_t refByte, std::string_view targetNs) noexcept
{
    if (!scope || targetNs.empty())
    {
        return false;
    }
    const IndexedOperatorSites* sites = scope->GetOperatorSites();
    if (!sites || sites->usings.empty())
    {
        return false;
    }
    for (const auto& u : sites->usings)
    {
        if (u.startByte <= refByte && refByte <= u.scopeEndByte && u.namespaceName == targetNs)
        {
            return true;
        }
    }
    return false;
}

bool MatchesClassTypeQualifier(std::string_view qualifier, const Scope* scope, uint32_t refByte,
                               const ReferenceCollectionCriteria& criteria)
{
    if (qualifier.empty() || !scope)
    {
        return false;
    }
    for (const auto& cls : criteria.compatibleClasses)
    {
        if (MatchesReferenceQualifier(qualifier, cls, *scope))
        {
            return true;
        }
    }
    const IndexedOperatorSites* sites = scope->GetOperatorSites();
    if (sites)
    {
        for (const auto& u : sites->usings)
        {
            if (u.startByte <= refByte && refByte <= u.scopeEndByte)
            {
                for (const auto& cls : criteria.compatibleClasses)
                {
                    if (MatchesScopedPair(cls, u.namespaceName, qualifier))
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static bool ContainsTypeSymbol(const SymbolTable& table, std::string_view container, std::string_view name)
{
    if (container.empty())
    {
        return false;
    }
    const auto found = table.FindMemberSymbolPtr(container, name);
    if (!found)
    {
        return false;
    }
    for (const auto& s : *found)
    {
        if (s.type == SymbolType::Funcdef || s.type == SymbolType::Typedef || s.type == SymbolType::Class ||
            s.type == SymbolType::Interface || s.type == SymbolType::Enum)
        {
            return true;
        }
    }
    return false;
}

bool IsGlobalTypeShadowed(const Scope* scope, const ReferenceCollectionCriteria& criteria)
{
    if (!criteria.isTypeTarget || !scope)
    {
        return false;
    }
    if (ContainsTypeSymbol(criteria.symbolTable, scope->GetEnclosingClass(), criteria.targetName))
    {
        return true;
    }
    bool shadowed = false;
    utils::ForEachEnclosingScope(scope->GetEnclosingNamespace(),
                                 [&](std::string_view ns) -> bool
                                 {
                                     if (ContainsTypeSymbol(criteria.symbolTable, ns, criteria.targetName))
                                     {
                                         shadowed = true;
                                         return true;
                                     }
                                     return false;
                                 });
    return shadowed;
}
} // namespace angel_lsp::analysis
