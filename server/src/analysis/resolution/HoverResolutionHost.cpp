#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Collects the members named `nodeText` across the host class and its bases, most derived first.
 * @param[in] hostClass Host class whose inheritance chain is searched.
 * @param[in] nodeText Member name being hovered.
 * @param[in] symbolTable Workspace symbol table.
 * @return Functions (an override hides the base overload with the same parameters), variables and properties.
 */
static std::vector<Symbol> CollectHierarchyHostSymbols(std::string_view hostClass, std::string_view nodeText,
                                                       const SymbolTable& symbolTable)
{
    std::vector<Symbol> hostSymbols;
    auto hierarchy = GetInheritedTypeHierarchy(hostClass, symbolTable);
    for (const auto& typeName : hierarchy)
    {
        auto found = symbolTable.FindMemberSymbolPtr(typeName, nodeText);
        if (found)
        {
            for (const auto& sym : *found)
            {
                if (sym.containerKind == ContainerKind::Namespace)
                {
                    continue;
                }
                if (sym.type == SymbolType::Function)
                {
                    bool overriddenLower = std::any_of(hostSymbols.begin(), hostSymbols.end(), [&](const Symbol& kept)
                                                       { return HasSameParameterList(kept, sym); });
                    if (!overriddenLower)
                    {
                        hostSymbols.push_back(sym);
                    }
                }
                else if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                {
                    hostSymbols.push_back(sym);
                }
            }
        }
    }
    return hostSymbols;
}

/**
 * @brief Keeps only container members and same-file symbols, then appends host symbols not already present.
 * @param[in,out] symbols Candidate list being narrowed and extended.
 * @param[in] hostSymbols Host-class symbols to merge, consumed.
 * @param[in] fileUri Hovered document URI.
 */
static void MergeHostSymbols(std::vector<Symbol>& symbols, std::vector<Symbol> hostSymbols, std::string_view fileUri)
{
    std::vector<Symbol> containerSymbols;
    for (const auto& s : symbols)
    {
        if (!s.containerName.empty() || s.fileUri == fileUri)
        {
            containerSymbols.push_back(s);
        }
    }
    symbols = std::move(containerSymbols);

    for (auto& hs : hostSymbols)
    {
        bool present = std::any_of(symbols.begin(), symbols.end(),
                                   [&](const Symbol& s) { return s.name == hs.name && HasSameParameterList(s, hs); });
        if (!present)
        {
            symbols.push_back(std::move(hs));
        }
    }
}

void AppendVirtualHostSymbols(const VirtualHostSymbolQuery& query, HoverCandidateSymbols& candidates)
{
    std::string hostClass;
    if (!query.virtualHostClass.empty())
    {
        hostClass = query.virtualHostClass;
    }
    else if (query.nodeIndex)
    {
        hostClass =
            InferHostClassFromEnclosingMixin(query.targetRange, *query.nodeIndex, query.sourceCode, query.symbolTable);
    }
    if (hostClass.empty())
    {
        return;
    }

    std::vector<Symbol> hostSymbols = CollectHierarchyHostSymbols(hostClass, query.nodeText, query.symbolTable);

    if (hostSymbols.empty())
    {
        auto hierarchy = GetInheritedTypeHierarchy(hostClass, query.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            auto accessors = FindPropertyAccessors(typeName, query.nodeText, query.symbolTable, false);
            if (!accessors.empty())
            {
                hostSymbols = std::move(accessors);
                candidates.accessorPropertyType = PropertyTypeFromAccessors(hostSymbols);
                break;
            }
        }
    }

    if (!hostSymbols.empty())
    {
        MergeHostSymbols(candidates.symbols, std::move(hostSymbols), query.fileUri);
    }
}

std::string TryHoverDelegateProperty(const VirtualHostSymbolQuery& query, const config::ServerConfig* config)
{
    const auto* engineRules = config ? &config->engineRules : nullptr;
    if (!IsBaseClassDelegateProperty(query.nodeText, query.symbolTable, engineRules))
    {
        return "";
    }
    std::string baseClass = ResolveDelegateTargetBaseClass(
        {query.targetRange, query.nodeIndex, query.sourceCode, query.symbolTable, query.virtualHostClass, engineRules});
    if (!baseClass.empty())
    {
        std::string result;
        result.reserve(32 + baseClass.size() + query.nodeText.size());
        result.append("```angelscript\n");
        result.append(baseClass);
        result.append("@ ");
        result.append(query.nodeText);
        result.append("\n```");
        return result;
    }
    return "";
}
} // namespace angel_lsp::analysis
