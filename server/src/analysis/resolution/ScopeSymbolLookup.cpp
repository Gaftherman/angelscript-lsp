#include "analysis/resolution/ScopeSymbolLookup.h"

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Matches qualified types by short name suffix against the rule index.
 * @param[in] clean Type name without root qualifier.
 * @param[in] index Precomputed rule index.
 * @return True if clean matches a known qualified type or suffix.
 */
static bool MatchesQualifiedTypeSuffix(std::string_view clean, const rules::RuleIndex* index)
{
    if (!index)
    {
        return false;
    }
    auto it = index->qualifiedTypesByShortName.find(LastScopeSegment(clean));
    if (it == index->qualifiedTypesByShortName.end())
    {
        return false;
    }
    const bool isPartiallyQualified = utils::ContainsScopeSeparator(clean);
    for (const auto& qType : it->second)
    {
        if (qType == clean || (isPartiallyQualified && utils::EndsWithScopedMember(qType, clean)))
        {
            return true;
        }
    }
    return false;
}

bool MatchesParentNamespaceType(std::string_view clean, const SymbolTable& table, const rules::RuleIndex* ruleIndex)
{
    const auto [parent, member] = utils::SplitScopedName(clean);
    if (parent.empty())
    {
        return false;
    }

    std::string_view prefix = parent;
    while (!prefix.empty())
    {
        prefix = utils::ParentScopeView(prefix);
        const std::string candidate = utils::JoinScopedName(prefix, member);
        if (table.HasSymbol(candidate) || table.HasSymbolAnywhere(candidate) ||
            MatchesQualifiedTypeSuffix(candidate, ruleIndex))
        {
            return true;
        }
    }
    return false;
}

bool IsReachableViaUsingDirectives(std::string_view clean, const DiagnosticContext& ctx)
{
    const auto& usings = GetUsingNamespaces(ctx.request);
    for (const auto& ns : usings)
    {
        std::string q = utils::JoinScopedName(ns, clean);
        if (ctx.request.symbolTable.HasSymbol(q))
        {
            return true;
        }
    }
    return false;
}

bool IsReachableInContainerScope(std::string_view clean, std::string_view containerScope, const SymbolTable& table)
{
    return table.IsReachableInContainerScope(clean, containerScope);
}

std::string EnclosingNamespaceForSymbol(const Symbol& sym, const SymbolTable& table)
{
    return std::string(EnclosingNamespaceView(sym, table));
}

std::string_view EnclosingNamespaceView(const Symbol& sym, const SymbolTable& table)
{
    if (sym.containerKind == ContainerKind::Namespace)
    {
        return sym.containerName;
    }
    if (sym.containerKind == ContainerKind::Class || sym.containerKind == ContainerKind::Interface)
    {
        if (auto classSyms = table.FindSymbolsPtr(sym.containerName))
        {
            for (const auto& cs : *classSyms)
            {
                if (cs.type == SymbolType::Class || cs.type == SymbolType::Interface)
                {
                    if (cs.containerKind == ContainerKind::Namespace)
                    {
                        return cs.containerName;
                    }
                    break;
                }
            }
        }
    }
    return {};
}

bool IsTemplateParameterOfContainer(std::string_view typeName, const Symbol& sym, const SymbolTable& table)
{
    if (typeName.empty() || sym.containerName.empty())
    {
        return false;
    }
    const std::string_view cleanContainer = CleanBaseTypeView(sym.containerName);
    const auto containerSyms = table.FindSymbolsPtr(cleanContainer.empty() ? sym.containerName : cleanContainer);
    if (!containerSyms)
    {
        return false;
    }
    for (const auto& owner : *containerSyms)
    {
        if (owner.type == SymbolType::Class && std::holds_alternative<ClassSignature>(owner.signature))
        {
            const auto& sig = owner.GetClass();
            if (std::find(sig.templateParams.begin(), sig.templateParams.end(), typeName) != sig.templateParams.end())
            {
                return true;
            }
        }
    }
    return false;
}

bool IsKnownType(std::string_view baseName, const DiagnosticContext& ctx, std::string_view containerScope)
{
    if (ctx.logger && ctx.logger->IsTraceEnabled())
    {
        ctx.logger->LogTrace(fmt::format("[SemanticHelpers] IsKnownType: checking '{}'", baseName));
    }
    if (baseName.empty() || IsCorePrimitive(baseName))
        return true;
    if (!ctx.request.GetStringTypeName().empty() && baseName == ctx.request.GetStringTypeName())
        return true;
    if (!ctx.request.GetArrayTypeName().empty() && baseName == ctx.request.GetArrayTypeName())
        return true;
    if (ctx.request.IsRegisteredSymbol(baseName))
        return true;

    std::string_view clean = utils::StripGlobalPrefix(baseName);
    if (ctx.request.symbolTable.HasSymbol(clean))
        return true;

    if (IsReachableInContainerScope(clean, containerScope, ctx.request.symbolTable))
        return true;

    const auto* ruleIndex = ctx.request.symbolTable.GetRuleIndex().get();
    if (MatchesQualifiedTypeSuffix(clean, ruleIndex))
        return true;

    if (MatchesParentNamespaceType(clean, ctx.request.symbolTable, ruleIndex))
        return true;

    return IsReachableViaUsingDirectives(clean, ctx);
}

bool ResolvesToEnum(std::string_view typeName, const SymbolTable& table)
{
    if (typeName.empty())
    {
        return false;
    }
    const std::string_view bare = LastScopeSegment(typeName);
    for (const std::string_view candidate : {typeName, bare})
    {
        const auto bucket = table.FindSymbolsPtr(candidate);
        if (bucket)
        {
            for (const auto& sym : *bucket)
            {
                if (sym.type == SymbolType::Enum)
                {
                    return true;
                }
            }
        }
        if (bare == typeName)
        {
            break;
        }
    }
    return false;
}

std::vector<ContainerInfo> GetEnclosingContainers(IndexedByteRange range, const NodeIndex& nodeIndex,
                                                  std::string_view sourceCode)
{
    return nodeIndex.CollectLookupContainers(range.startByte, range.endByte, 0, sourceCode);
}

std::vector<std::string> CollectUsingNamespaces(const NodeIndex& nodeIndex, std::string_view sourceCode)
{
    return nodeIndex.CollectUsingNamespaces(sourceCode);
}

const std::vector<std::string>& GetUsingNamespaces(const SemanticAnalysisRequest& request)
{
    if (!request.usingNamespacesCache.has_value())
    {
        if (request.nodeIndex)
        {
            request.usingNamespacesCache = CollectUsingNamespaces(*request.nodeIndex, request.sourceCode);
        }
        else
        {
            request.usingNamespacesCache = std::vector<std::string>{};
        }
    }
    return *request.usingNamespacesCache;
}

/**
 * @brief Filters candidate symbols to ensure they match the expected container kind.
 * @param[in] found Symbol candidates to filter.
 * @param[in] expected Expected enclosing container kind.
 * @return Filtered vector containing only symbols consistent with expected container.
 */
std::vector<Symbol> FilterSymbolsByContainerKind(std::vector<Symbol> found, ContainerKind expected)
{
    std::erase_if(found,
                  [expected](const Symbol& s)
                  {
                      if (s.type == SymbolType::CallReference)
                      {
                          return true;
                      }
                      if (expected == ContainerKind::Class || expected == ContainerKind::Interface)
                      {
                          return s.containerKind == ContainerKind::Namespace || s.containerKind == ContainerKind::Enum;
                      }
                      if (expected == ContainerKind::Namespace)
                      {
                          return s.containerKind == ContainerKind::Class || s.containerKind == ContainerKind::Interface;
                      }
                      return false;
                  });
    return found;
}

static std::vector<Symbol> FindEnumMemberInNamespace(const SymbolTable& symbolTable, std::string_view namespaceName,
                                                     std::string_view name)
{
    const auto ruleIndex = symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return {};
    }
    auto it = ruleIndex->enumSymbolsByMemberName.find(name);
    if (it == ruleIndex->enumSymbolsByMemberName.end())
    {
        return {};
    }
    for (const auto& enumSym : it->second)
    {
        if (enumSym.containerName == namespaceName || IsSameOrNestedNamespace(enumSym.containerName, namespaceName))
        {
            auto enumMember = symbolTable.FindMemberSymbols(enumSym.qualifiedName, name);
            if (!enumMember.empty())
            {
                return enumMember;
            }
        }
    }
    return {};
}

static std::vector<Symbol> FindSymbolInClassHierarchy(const SymbolTable& symbolTable, const ContainerInfo& container,
                                                      std::string_view name)
{
    auto hierarchy = GetInheritedTypeHierarchy(container.qualifiedName, symbolTable);
    ExpandMixinHostHierarchies(container.qualifiedName, symbolTable, hierarchy);
    for (const auto& cls : hierarchy)
    {
        auto found = FilterSymbolsByContainerKind(symbolTable.FindMemberSymbols(cls, name), container.kind);
        if (!found.empty())
        {
            return found;
        }
    }

    if (container.name != container.qualifiedName)
    {
        auto bareHierarchy = GetInheritedTypeHierarchy(container.name, symbolTable);
        ExpandMixinHostHierarchies(container.name, symbolTable, bareHierarchy);
        for (const auto& cls : bareHierarchy)
        {
            auto found = FilterSymbolsByContainerKind(symbolTable.FindMemberSymbols(cls, name), container.kind);
            if (!found.empty())
            {
                return found;
            }
        }
    }
    return {};
}

/**
 * @brief Searches for a symbol name within an enclosing container hierarchy.
 */
static std::vector<Symbol> FindSymbolInContainerHierarchy(const SymbolTable& symbolTable,
                                                          const std::vector<ContainerInfo>& containers,
                                                          std::string_view name)
{
    ankerl::unordered_dense::set<std::string_view> visitedContainers;
    for (const auto& container : containers)
    {
        if (!visitedContainers.insert(container.qualifiedName).second)
        {
            continue;
        }

        if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            auto found = FindSymbolInClassHierarchy(symbolTable, container, name);
            if (!found.empty())
            {
                return found;
            }
        }
        else if (container.kind == ContainerKind::Namespace)
        {
            auto found = FilterSymbolsByContainerKind(symbolTable.FindMemberSymbols(container.qualifiedName, name),
                                                      container.kind);
            if (!found.empty())
            {
                return found;
            }
            auto enumMember = FindEnumMemberInNamespace(symbolTable, container.qualifiedName, name);
            if (!enumMember.empty())
            {
                return enumMember;
            }
        }
    }
    return {};
}

/**
 * @brief Searches for a symbol name across using namespaces.
 */
static std::vector<Symbol> FindSymbolInUsingNamespaces(const SymbolTable& symbolTable,
                                                       std::span<const std::string> usingNamespaces,
                                                       std::string_view name)
{
    for (const auto& ns : usingNamespaces)
    {
        auto found = symbolTable.FindMemberSymbols(ns, name);
        if (!found.empty())
        {
            return found;
        }
        auto enumMember = FindEnumMemberInNamespace(symbolTable, ns, name);
        if (!enumMember.empty())
        {
            return enumMember;
        }
    }
    return {};
}

/**
 * @brief Fallback lookup for enum members and short-name types.
 */
static std::vector<Symbol> FindSymbolEnumOrTypeFallback(const SymbolTable& symbolTable, std::string_view name)
{
    if (const auto ruleIndex = symbolTable.GetRuleIndex())
    {
        auto it = ruleIndex->enumSymbolsByMemberName.find(name);
        if (it != ruleIndex->enumSymbolsByMemberName.end())
        {
            return it->second;
        }
    }
    return symbolTable.FindTypeSymbolsByShortName(name);
}

std::vector<Symbol> FindSymbolsInScope(const SymbolTable& symbolTable, const std::vector<ContainerInfo>& containers,
                                       std::string_view name, std::span<const std::string> usingNamespaces)
{
    if (name.empty())
    {
        return {};
    }

    if (utils::HasLeadingGlobalPrefix(name))
    {
        const std::string_view rest = utils::StripGlobalPrefix(name);
        if (!utils::ContainsScopeSeparator(rest))
        {
            std::vector<Symbol> filtered;
            if (auto symsPtr = symbolTable.FindSymbolsPtr(rest))
            {
                for (const auto& sym : *symsPtr)
                {
                    if (sym.containerName.empty() && sym.type != SymbolType::CallReference)
                    {
                        filtered.push_back(sym);
                    }
                }
            }
            return filtered;
        }
        return symbolTable.FindSymbols(rest);
    }

    if (auto found = FindSymbolInContainerHierarchy(symbolTable, containers, name); !found.empty())
    {
        return found;
    }

    if (auto globalFound = symbolTable.FindSymbols(name); !globalFound.empty())
    {
        return globalFound;
    }

    if (auto found = FindSymbolInUsingNamespaces(symbolTable, usingNamespaces, name); !found.empty())
    {
        return found;
    }

    return FindSymbolEnumOrTypeFallback(symbolTable, name);
}

std::vector<std::string> CollectVisibleUsingNamespaces(uint32_t byteOffset, std::string_view sourceCode,
                                                       const NodeIndex* nodeIndex)
{
    if (nodeIndex)
    {
        return nodeIndex->CollectUsingNamespaces(sourceCode, byteOffset);
    }
    return {};
}

std::vector<Symbol> FindSymbolsInScope(std::string_view name, uint32_t byteOffset, std::string_view sourceCode,
                                       const SymbolTable& symbolTable, const NodeIndex* nodeIndex)
{
    if (name.empty())
    {
        return {};
    }
    std::vector<ContainerInfo> containers;
    if (nodeIndex)
    {
        containers = GetEnclosingContainers(IndexedByteRange{byteOffset, byteOffset}, *nodeIndex, sourceCode);
    }
    return FindSymbolsInScope(symbolTable, containers, name,
                              CollectVisibleUsingNamespaces(byteOffset, sourceCode, nodeIndex));
}

bool IsVariableType(std::string_view typeName)
{
    // Strip whatever decoration the declaration carried: const, &, @, in/out/inout, spaces.
    // What has to remain is a bare '?' and nothing else.
    std::string cleaned;
    cleaned.reserve(typeName.size());
    for (const char c : typeName)
    {
        if (c == '?')
            cleaned.push_back(c);
        else if (c != ' ' && c != '\t' && c != '&' && c != '@')
        {
            // Any other identifier character means this is a real type, not the wildcard.
            // "const" / "in" / "out" / "inout" are the only words legally adjacent to it.
            cleaned.push_back(c);
        }
    }

    static constexpr std::string_view k_decorations[] = {"const", "inout", "out", "in"};
    for (const auto decoration : k_decorations)
    {
        for (size_t at = cleaned.find(decoration); at != std::string::npos; at = cleaned.find(decoration, at))
            cleaned.erase(at, decoration.size());
    }

    return cleaned == "?";
}

std::vector<std::pair<uint32_t, uint32_t>> CollectNamespaceAndContainerLineRanges(const SymbolTable& symbolTable,
                                                                                  std::string_view fileUri,
                                                                                  std::string_view declaringNamespace)
{
    std::vector<std::pair<uint32_t, uint32_t>> nsRanges;
    if (declaringNamespace.empty())
    {
        return nsRanges;
    }
    symbolTable.ForEachSymbolInFile(
        fileUri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& sList)
        {
            for (const auto& s : sList)
            {
                if (s.fileUri != fileUri)
                {
                    continue;
                }
                if (s.type == analysis::SymbolType::Namespace &&
                    (s.name == declaringNamespace || s.qualifiedName == declaringNamespace))
                {
                    nsRanges.push_back({s.startLine, s.endLine});
                }
                else if ((s.type == analysis::SymbolType::Class || s.type == analysis::SymbolType::Interface) &&
                         IsSameOrNestedNamespace(s.containerName, declaringNamespace))
                {
                    nsRanges.push_back({s.startLine, s.endLine});
                }
            }
        });
    return nsRanges;
}

bool NamesAFunctionNotAType(std::string_view name, const SymbolTable& table)
{
    if (name.empty())
    {
        return false;
    }

    const auto symsPtr = table.FindSymbolsPtr(name);
    if (!symsPtr)
    {
        return false;
    }

    bool isFunction = false;
    for (const auto& sym : *symsPtr)
    {
        switch (sym.type)
        {
        case SymbolType::Function:
            if (sym.containerName.empty())
            {
                isFunction = true;
            }
            break;

        case SymbolType::Class:
        case SymbolType::Interface:
        case SymbolType::Enum:
        case SymbolType::Typedef:
        case SymbolType::Funcdef:
            return false;

        default:
            break;
        }
    }

    return isFunction;
}

std::vector<Symbol> LookupTypeCandidateSymbols(std::string_view name, const SymbolTable& symbolTable)
{
    auto syms = symbolTable.FindSymbols(name);
    if (syms.empty())
    {
        const std::string_view shortName = LastScopeSegment(name);
        if (shortName != name)
        {
            syms = symbolTable.FindSymbols(shortName);
        }
    }
    if (syms.empty())
    {
        auto typeSyms = symbolTable.FindTypeSymbolsByShortName(LastScopeSegment(name));
        if (!typeSyms.empty())
        {
            syms = std::move(typeSyms);
        }
    }
    return syms;
}

bool AreAllSymbolsPureTypes(const std::vector<Symbol>& syms)
{
    if (syms.empty())
    {
        return false;
    }
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Variable || s.type == SymbolType::Function || s.type == SymbolType::Property)
        {
            return false;
        }
    }
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Class || s.type == SymbolType::Interface || s.type == SymbolType::Enum ||
            s.type == SymbolType::Typedef || s.type == SymbolType::Funcdef)
        {
            return true;
        }
    }
    return false;
}

std::optional<std::string> IsBareDataType(std::string_view name, uint32_t byteOffset, const ExpressionTypeContext& ctx)
{
    if (name.empty() || name.ends_with(')') || name.find('(') != std::string_view::npos ||
        name.find('=') != std::string_view::npos || name.find('{') != std::string_view::npos ||
        name.find('}') != std::string_view::npos)
    {
        return std::nullopt;
    }

    if (IsPrimitiveTypeName(name))
    {
        return std::string(name);
    }

    if (ctx.scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(ctx.scope, name, byteOffset))
        {
            return def->kind == LocalDefinitionKind::Type ? std::optional<std::string>(std::string(name))
                                                          : std::nullopt;
        }
    }

    const auto syms = LookupTypeCandidateSymbols(name, ctx.symbolTable);
    if (AreAllSymbolsPureTypes(syms))
    {
        return std::string(name);
    }

    return std::nullopt;
}

} // namespace angel_lsp::analysis
