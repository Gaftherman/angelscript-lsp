#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

bool MemberBelongsToClass(const Symbol& classSym, std::string_view memberName, const SymbolTable& table)
{
    const std::string_view qName =
        classSym.qualifiedName.empty() ? std::string_view(classSym.name) : std::string_view(classSym.qualifiedName);
    auto hierarchy = analysis::GetInheritedTypeHierarchy(qName, table);
    for (const auto& cls : hierarchy)
    {
        if (const auto found = table.FindMemberSymbolPtr(cls, memberName))
        {
            for (const auto& sym : *found)
            {
                if (sym.containerKind == ContainerKind::Class || sym.containerKind == ContainerKind::Interface)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool MemberBelongsToNamespace(const Symbol& nsSym, std::string_view memberName, const SymbolTable& table)
{
    const std::string_view qName =
        nsSym.qualifiedName.empty() ? std::string_view(nsSym.name) : std::string_view(nsSym.qualifiedName);
    if (const auto found = table.FindMemberSymbolPtr(qName, memberName))
    {
        for (const auto& sym : *found)
        {
            if (sym.containerKind == ContainerKind::Namespace)
            {
                return true;
            }
        }
    }
    return false;
}

void AppendEnclosingClassMethods(IndexedByteRange targetRange, std::string_view nodeText,
                                 const ExpressionTypeContext& ctx, std::vector<Symbol>& symbols)
{
    std::vector<ContainerInfo> containers;
    if (ctx.nodeIndex)
    {
        containers = analysis::GetEnclosingContainers(targetRange, *ctx.nodeIndex, ctx.sourceCode);
    }

    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            auto hierarchy = analysis::GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName,
                                                                 ctx.symbolTable);
            for (const auto& typeName : hierarchy)
            {
                auto found = ctx.symbolTable.FindMemberSymbolPtr(typeName, nodeText);
                if (found)
                {
                    for (const auto& sym : *found)
                    {
                        if (sym.type == SymbolType::Function && sym.containerKind != ContainerKind::Namespace)
                        {
                            bool overriddenLower = std::any_of(symbols.begin(), symbols.end(), [&](const Symbol& kept)
                                                               { return analysis::HasSameParameterList(kept, sym); });
                            if (!overriddenLower)
                            {
                                symbols.push_back(sym);
                            }
                        }
                    }
                }
            }
            break;
        }
    }
}

/**
 * @brief Name lookup at the hover target: the byte-range form of `FindSymbolsInScope(name, node, ...)`.
 * @param[in] query Hover site, source text, symbol table and node index.
 * @param[in] name Plain or scoped name to look up.
 * @return Symbols visible from the target's containers and namespace imports; empty for an empty name.
 * @note Passes symbol 0 to CollectLookupContainers: the target is an identifier, so it never matches a
 *       container declaration, the script root or a base-class list, the only cases the symbol selects.
 */
static std::vector<Symbol> FindSymbolsAtHoverSite(const ScopedSymbolQuery& query, std::string_view name)
{
    if (name.empty())
    {
        return {};
    }
    if (!query.nodeIndex)
    {
        return FindSymbolsInScope(query.symbolTable, {}, name);
    }
    return FindSymbolsInScope(query.symbolTable,
                              query.nodeIndex->CollectLookupContainers(query.site.targetStartByte,
                                                                       query.site.targetEndByte, 0, query.sourceCode),
                              name,
                              query.nodeIndex->CollectUsingNamespaces(query.sourceCode, query.site.targetStartByte));
}

static void ResolveScopedMemberFallback(std::string_view scopedPrefix, const ScopedSymbolQuery& query,
                                        std::vector<Symbol>& symbols)
{
    if (!symbols.empty() || !utils::ContainsScopeSeparator(scopedPrefix))
    {
        return;
    }
    const std::string_view qualifierView = utils::ParentScopeView(scopedPrefix);
    if (qualifierView.empty())
    {
        return;
    }
    auto qualSyms = FindSymbolsAtHoverSite(query, qualifierView);
    for (const auto& qs : qualSyms)
    {
        const std::string_view qName =
            qs.qualifiedName.empty() ? std::string_view(qs.name) : std::string_view(qs.qualifiedName);
        if (const auto found = query.symbolTable.FindMemberSymbolPtr(qName, query.nodeText))
        {
            symbols.insert(symbols.end(), found->begin(), found->end());
            return;
        }
    }
}

std::vector<Symbol> CollectHoverScopedSymbols(const ScopedSymbolQuery& query, std::string_view& outPathText)
{
    std::vector<Symbol> symbols;
    bool isExplicitlyScoped = false;
    const IndexedByteRange qualifier = query.site.scopeQualifierRange;
    const IndexedByteRange typeScope = query.site.datatypeScopeRange;
    if (!qualifier.Empty() && qualifier.endByte <= query.sourceCode.size())
    {
        const std::string_view scopedPrefix =
            query.sourceCode.substr(qualifier.startByte, qualifier.endByte - qualifier.startByte);
        outPathText = scopedPrefix;
        symbols = FindSymbolsAtHoverSite(query, scopedPrefix);
        ResolveScopedMemberFallback(scopedPrefix, query, symbols);
        isExplicitlyScoped = utils::ContainsScopeSeparator(scopedPrefix);
    }
    else if (!typeScope.Empty() && typeScope.endByte <= query.sourceCode.size())
    {
        std::string prefix(query.sourceCode.substr(typeScope.startByte, typeScope.endByte - typeScope.startByte));
        if (!utils::EndsWithScopeSeparator(prefix))
        {
            prefix.append(utils::k_scopeSeparator);
        }
        outPathText = query.sourceCode.substr(typeScope.startByte, query.site.targetEndByte - typeScope.startByte);
        symbols = FindSymbolsAtHoverSite(query, utils::JoinScopedName(prefix, query.nodeText));
        isExplicitlyScoped = true;
    }

    if (symbols.empty() && !isExplicitlyScoped)
    {
        symbols = FindSymbolsAtHoverSite(query, query.nodeText);
    }

    if (!query.site.enclosingEnumNameRange.Empty())
    {
        const uint32_t eStart = query.site.enclosingEnumNameRange.startByte;
        const uint32_t eEnd = query.site.enclosingEnumNameRange.endByte;
        if (eStart < query.sourceCode.size() && eEnd <= query.sourceCode.size() && eStart < eEnd)
        {
            const std::string_view enumName = query.sourceCode.substr(eStart, eEnd - eStart);
            std::erase_if(symbols,
                          [&](const Symbol& s)
                          {
                              if (s.containerName == enumName || utils::EndsWithScopedMember(s.containerName, enumName))
                              {
                                  return false;
                              }
                              if (std::holds_alternative<VariableSignature>(s.signature))
                              {
                                  const auto& vs = s.GetVariable();
                                  if (vs.isEnumConstant &&
                                      (vs.typeName == enumName || utils::EndsWithScopedMember(vs.typeName, enumName)))
                                  {
                                      return false;
                                  }
                              }
                              return true;
                          });
        }
    }

    std::erase_if(symbols, [](const Symbol& s) { return s.type == SymbolType::CallReference; });
    return symbols;
}

} // namespace angel_lsp::analysis
