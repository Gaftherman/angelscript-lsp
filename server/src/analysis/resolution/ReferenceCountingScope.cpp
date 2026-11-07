#include "analysis/resolution/ReferenceCountingScope.h"

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
        it = cache.emplace(std::string(uri), std::move(spans)).first;
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
} // namespace angel_lsp::analysis
