#include "analysis/resolution/ScopeSymbolLookup.h"

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "parser/QueryRegistry.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
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
    const std::string shortName = std::string(LastScopeSegment(clean));
    auto it = index->qualifiedTypesByShortName.find(shortName);
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
    return "";
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
    std::string bare = std::string(LastScopeSegment(typeName));
    for (const auto& candidate : {std::string(typeName), bare})
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

TSNode AdjustStartNodeIfDeclarationName(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode result = node;
    parser::ForEachAncestor(
        node,
        [&](TSNode cur) -> bool
        {
            const TSNode parent = parser::GetParent(cur);
            if (ts_node_is_null(parent))
            {
                return false;
            }
            const TSSymbol parentSym = ts_node_symbol(parent);
            if (parentSym == gs.symBaseClassList)
            {
                TSNode decl = parser::GetParent(parent);
                if (!ts_node_is_null(decl))
                {
                    result = decl;
                    return false;
                }
            }
            if (parentSym == gs.symClassDeclaration || parentSym == gs.symInterfaceDeclaration ||
                parentSym == gs.symNamespaceDeclaration || parentSym == gs.symMixinDeclaration)
            {
                const TSNode nameChild = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldName);
                if (!ts_node_is_null(nameChild) && ts_node_start_byte(nameChild) == ts_node_start_byte(node) &&
                    ts_node_end_byte(nameChild) == ts_node_end_byte(node))
                {
                    result = parent;
                    return false;
                }
            }
            return true;
        });
    return result;
}

std::vector<ContainerInfo> GetEnclosingContainers(TSNode node, std::string_view sourceCode, const NodeIndex* nodeIndex)
{
    if (ts_node_is_null(node))
    {
        return {};
    }

    if (nodeIndex)
    {
        return nodeIndex->CollectLookupContainers(ts_node_start_byte(node), ts_node_end_byte(node),
                                                  ts_node_symbol(node), sourceCode);
    }

    std::vector<ContainerInfo> rawContainers;
    bool crossedLambda = false;
    const auto& gs = parser::GetGrammarSymbols();
    parser::ForEachAncestor(
        parser::GetParent(AdjustStartNodeIfDeclarationName(node)),
        [&](TSNode current)
        {
            if (ts_node_symbol(current) == gs.symLambdaExpression)
            {
                crossedLambda = true;
            }

            if (const auto kind = ClassifyContainerKind(ts_node_symbol(current)))
            {
                if (crossedLambda && (*kind == ContainerKind::Class || *kind == ContainerKind::Interface))
                {
                    return;
                }

                AppendRawContainers(parser::GetChildByField(current, parser::GetGrammarSymbols().fieldName), *kind,
                                    sourceCode, rawContainers);
            }
        });

    ComputeContainerQualifiedNames(rawContainers);
    return rawContainers;
}

std::vector<std::string> CollectUsingNamespaces(const NodeIndex& nodeIndex, std::string_view sourceCode)
{
    return nodeIndex.CollectUsingNamespaces(sourceCode);
}

std::vector<std::string> CollectUsingNamespaces(TSNode root, std::string_view sourceCode)
{
    std::vector<std::string> usings;
    if (ts_node_is_null(root) || sourceCode.find("using") == std::string_view::npos)
    {
        return usings;
    }

    const TSQuery* query = parser::QueryRegistry::GetUsingQuery();
    TSQueryCursor* cursor = parser::QueryRegistry::GetThreadLocalCursor();
    if (!query || !cursor)
    {
        return usings;
    }

    ts_query_cursor_exec(cursor, query, root);
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match))
    {
        for (uint16_t i = 0; i < match.capture_count; ++i)
        {
            TSNode nameNode = match.captures[i].node;
            std::string uName(parser::NodeText(nameNode, sourceCode));
            while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front())))
                uName.erase(uName.begin());
            while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back())))
                uName.pop_back();
            if (!uName.empty())
            {
                usings.push_back(std::move(uName));
            }
        }
    }
    return usings;
}

const std::vector<std::string>& GetUsingNamespaces(const SemanticAnalysisRequest& request)
{
    if (!request.usingNamespacesCache.has_value())
    {
        if (request.nodeIndex)
        {
            request.usingNamespacesCache = CollectUsingNamespaces(*request.nodeIndex, request.sourceCode);
        }
        else if (request.tree)
        {
            request.usingNamespacesCache = CollectUsingNamespaces(ts_tree_root_node(request.tree), request.sourceCode);
        }
        else
        {
            request.usingNamespacesCache = std::vector<std::string>{};
        }
    }
    return *request.usingNamespacesCache;
}

/**
 * @brief Checks if any symbol or container in the symbol table matches the scope prefix.
 */
static bool HasContainerMatchingPrefix(const SymbolTable& table, const std::string& prefix)
{
    if (const auto ruleIndex = table.GetRuleIndex())
    {
        if (ruleIndex->byContainer.contains(prefix))
        {
            return true;
        }
        for (const auto& [container, _] : ruleIndex->byContainer)
        {
            if (utils::HasScopePrefix(container, prefix))
            {
                return true;
            }
        }
        return false;
    }
    return table.HasSymbol(prefix) || table.HasSymbolAnywhere(prefix);
}

/**
 * @brief Checks if the prefix is known relative to enclosing containers.
 */
static bool IsKnownInEnclosingContainers(const SymbolTable& table, const std::string& prefix, TSNode node,
                                         std::string_view sourceCode, const NodeIndex* nodeIndex)
{
    auto containers = GetEnclosingContainers(node, sourceCode, nodeIndex);
    for (const auto& c : containers)
    {
        std::string q = utils::JoinScopedName(c.qualifiedName, prefix);
        if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if the prefix is known relative to using namespaces.
 */
static bool IsKnownInUsingNamespaces(const SymbolTable& table, const std::string& prefix, TSNode node,
                                     std::string_view sourceCode, std::span<const std::string> precomputedUsings,
                                     const NodeIndex* nodeIndex)
{
    if (nodeIndex || !precomputedUsings.empty())
    {
        for (const auto& ns : precomputedUsings)
        {
            std::string q = utils::JoinScopedName(ns, prefix);
            if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
            {
                return true;
            }
        }
        return false;
    }

    if (sourceCode.find("using") == std::string_view::npos)
    {
        return false;
    }

    auto usings = CollectUsingNamespaces(parser::GetRootNode(node), sourceCode);
    for (const auto& ns : usings)
    {
        std::string q = utils::JoinScopedName(ns, prefix);
        if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
        {
            return true;
        }
    }
    return false;
}

bool IsKnownScope(const std::string& prefix, TSNode node, std::string_view sourceCode, const SymbolTable& table,
                  std::span<const std::string> precomputedUsings, const NodeIndex* nodeIndex)
{
    if (prefix.empty() || table.HasSymbol(prefix) || table.HasSymbolAnywhere(prefix))
    {
        return true;
    }

    if (HasContainerMatchingPrefix(table, prefix))
    {
        return true;
    }

    if (IsKnownInEnclosingContainers(table, prefix, node, sourceCode, nodeIndex))
    {
        return true;
    }

    return IsKnownInUsingNamespaces(table, prefix, node, sourceCode, precomputedUsings, nodeIndex);
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

static std::vector<Symbol> FindEnumMemberInNamespace(const SymbolTable& symbolTable, const std::string& namespaceName,
                                                     const std::string& name)
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
                                                      const std::string& name)
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
                                                          const std::string& name)
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
                                                       const std::vector<std::string>& usingNamespaces,
                                                       const std::string& name)
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
static std::vector<Symbol> FindSymbolEnumOrTypeFallback(const SymbolTable& symbolTable, const std::string& name)
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
                                       const std::string& name, const std::vector<std::string>& usingNamespaces)
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
            if (auto symsPtr = symbolTable.FindSymbolsPtr(std::string(rest)))
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

static void CollectUsingsFromAncestor(TSNode p, std::string_view sourceCode, std::vector<std::string>& usings)
{
    TSTreeCursor cursor = ts_tree_cursor_new(p);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            const auto& gs = parser::GetGrammarSymbols();
            if (ts_node_symbol(ch) == gs.symUsingDeclaration)
            {
                TSNode nameNode = parser::GetChildByField(ch, gs.fieldName);
                if (!ts_node_is_null(nameNode))
                {
                    std::string uName(parser::NodeText(nameNode, sourceCode));
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front())))
                        uName.erase(uName.begin());
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back())))
                        uName.pop_back();
                    if (!uName.empty())
                    {
                        usings.push_back(uName);
                    }
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

std::vector<std::string> CollectVisibleUsingNamespaces(TSNode node, std::string_view sourceCode,
                                                       const NodeIndex* nodeIndex)
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    if (nodeIndex)
    {
        return nodeIndex->CollectUsingNamespaces(sourceCode, ts_node_start_byte(node));
    }

    std::vector<std::string> usings;
    if (sourceCode.find("using") != std::string_view::npos)
    {
        parser::ForEachAncestor(node, [&](TSNode p) { CollectUsingsFromAncestor(p, sourceCode, usings); });
    }
    return usings;
}

std::vector<Symbol> FindSymbolsInScope(const std::string& name, TSNode node, std::string_view sourceCode,
                                       const SymbolTable& symbolTable, const NodeIndex* nodeIndex)
{
    if (name.empty())
    {
        return {};
    }
    return FindSymbolsInScope(symbolTable, GetEnclosingContainers(node, sourceCode, nodeIndex), name,
                              CollectVisibleUsingNamespaces(node, sourceCode, nodeIndex));
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
                                                                                  const std::string& fileUri,
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

std::vector<Symbol> LookupTypeCandidateSymbols(const std::string& name, const SymbolTable& symbolTable)
{
    auto syms = symbolTable.FindSymbols(name);
    if (syms.empty())
    {
        const std::string shortName = std::string(LastScopeSegment(name));
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

static std::string ExtractTrimmedIdentifier(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym != gs.symIdentifier && sym != gs.symScopedIdentifier)
    {
        return {};
    }
    std::string name(parser::NodeText(node, sourceCode));
    while (!name.empty() && isspace(static_cast<unsigned char>(name.front())))
    {
        name.erase(name.begin());
    }
    while (!name.empty() && isspace(static_cast<unsigned char>(name.back())))
    {
        name.pop_back();
    }
    return name;
}

std::optional<std::string> IsBareDataType(TSNode node, const ExpressionTypeContext& ctx)
{
    node = UnwrapIndexedParentheses(node, ctx.nodeIndex);
    std::string name = ExtractTrimmedIdentifier(node, ctx.sourceCode);
    if (name.empty())
    {
        return std::nullopt;
    }

    if (IsPrimitiveTypeName(name))
    {
        return name;
    }

    if (ctx.scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(ctx.scope, name, ts_node_start_byte(node)))
        {
            return def->kind == LocalDefinitionKind::Type ? std::optional<std::string>(name) : std::nullopt;
        }
    }

    const auto syms = LookupTypeCandidateSymbols(name, ctx.symbolTable);
    if (AreAllSymbolsPureTypes(syms))
    {
        return name;
    }

    return std::nullopt;
}

std::string ExtractDatatypeScopePrefix(TSNode datatypeNode, std::string_view sourceCode)
{
    TSNode typeNode = parser::GetParent(datatypeNode);
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(typeNode) || ts_node_symbol(typeNode) != gs.symType)
    {
        return {};
    }
    TSTreeCursor cursor = ts_tree_cursor_new(typeNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode c = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(c) == gs.symScope)
            {
                const uint32_t sStart = ts_node_start_byte(c);
                const uint32_t sEnd = ts_node_end_byte(c);
                ts_tree_cursor_delete(&cursor);
                if (sStart < sourceCode.size() && sEnd <= sourceCode.size() && sStart < sEnd)
                {
                    std::string scopeText(sourceCode.substr(sStart, sEnd - sStart));
                    if (!utils::EndsWithScopeSeparator(scopeText))
                    {
                        scopeText.append(utils::k_scopeSeparator);
                    }
                    return scopeText;
                }
                return {};
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
        ts_tree_cursor_delete(&cursor);
    }
    return {};
}

} // namespace angel_lsp::analysis
