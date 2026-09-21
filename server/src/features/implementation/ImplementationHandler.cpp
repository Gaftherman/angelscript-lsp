#include "features/implementation/ImplementationHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "parser/GrammarNames.h"
#include "utils/LspLogger.h"
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
namespace
{
using analysis::Symbol;
using analysis::SymbolTable;
using analysis::SymbolType;
using analysis::rules::RuleIndex;

/** @brief The identifier the cursor sits on, or empty when it is not on one. */
std::string IdentifierAt(const ImplementationRequest& request, TSNode& outNode)
{
    outNode = TSNode{};
    if (!request.tree)
    {
        return "";
    }

    const TSNode root = ts_tree_root_node(request.tree);
    const TSPoint point{request.position.line, request.position.character};
    TSNode node = ts_node_descendant_for_point_range(root, point, point);
    if (ts_node_is_null(node) || std::string_view(ts_node_type(node)) != "identifier")
    {
        return "";
    }

    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > request.sourceCode.size())
    {
        return "";
    }

    outNode = node;
    return request.sourceCode.substr(start, end - start);
}

/** @brief Every base a declaration lists, whichever kind of declaration it is. */
std::vector<std::string> DeclaredBases(const Symbol& sym)
{
    if (sym.type == SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(sym.signature))
    {
        return sym.GetClass().bases;
    }
    if (sym.type == SymbolType::Interface && std::holds_alternative<analysis::InterfaceSignature>(sym.signature))
    {
        return sym.GetInterface().inheritedInterfaces;
    }
    return {};
}

/**
 * @brief Accumulator for subtype collection passes.
 */
struct SubtypeAccumulator
{
    ankerl::unordered_dense::set<std::string>& seen;
    std::vector<std::string>& next;
    std::vector<Symbol>& subtypes;
};

/**
 * @brief Expands derived types from a list of records and records matches in the accumulator.
 * @param[in] derivedList List of derived type records.
 * @param[in] table Symbol table.
 * @param[in,out] acc Subtype accumulator.
 */
template <typename TList>
void ExpandDerivedFromList(const TList& derivedList, const SymbolTable& table, SubtypeAccumulator& acc)
{
    for (const auto& derived : derivedList)
    {
        const std::string bare = analysis::LastScopeSegment(derived.name);
        if (!acc.seen.insert(bare).second)
        {
            continue;
        }
        if (!derived.qualifiedName.empty())
        {
            acc.seen.insert(derived.qualifiedName);
        }

        acc.next.push_back(bare);
        if (!derived.qualifiedName.empty() && derived.qualifiedName != bare)
        {
            acc.next.push_back(derived.qualifiedName);
        }

        const auto symList = table.FindSymbolsPtr(derived.qualifiedName.empty() ? derived.name : derived.qualifiedName);
        if (symList)
        {
            for (const Symbol& s : *symList)
            {
                if (s.type == SymbolType::Class || s.type == SymbolType::Interface)
                {
                    acc.subtypes.push_back(s);
                }
            }
        }
    }
}

/**
 * @brief Collects subtypes using precomputed rule index derived maps.
 * @param[in] rootType Root type name.
 * @param[in] table Symbol table.
 * @param[in] ruleIndex Precomputed rule index.
 * @return List of subtype symbols.
 */
std::vector<Symbol> CollectSubtypesFromRuleIndex(const std::string& rootType, const SymbolTable& table,
                                                 const RuleIndex& ruleIndex)
{
    const std::string bareRoot = analysis::LastScopeSegment(rootType);
    std::vector<std::string> frontier{bareRoot};
    if (bareRoot != rootType && !rootType.empty())
    {
        frontier.push_back(rootType);
    }
    ankerl::unordered_dense::set<std::string> seen{bareRoot, rootType};
    std::vector<Symbol> subtypes;

    while (!frontier.empty())
    {
        std::vector<std::string> next;
        SubtypeAccumulator acc{seen, next, subtypes};

        for (const auto& baseName : frontier)
        {
            const auto it = ruleIndex.derivedByBase.find(baseName);
            if (it != ruleIndex.derivedByBase.end())
            {
                ExpandDerivedFromList(it->second, table, acc);
            }

            const auto itHost = ruleIndex.hostClassesByMixin.find(baseName);
            if (itHost != ruleIndex.hostClassesByMixin.end())
            {
                ExpandDerivedFromList(itHost->second, table, acc);
            }
        }

        frontier = std::move(next);
    }
    return subtypes;
}

/**
 * @brief Checks if a symbol inherits or includes any mixins in the frontier.
 * @param[in] sym Candidate symbol.
 * @param[in] frontier Current frontier of base names.
 * @return True if sym matches the frontier.
 */
bool MatchesDeclaredBasesOrMixins(const Symbol& sym, const std::vector<std::string>& frontier)
{
    for (const auto& base : DeclaredBases(sym))
    {
        const std::string baseName = analysis::LastScopeSegment(analysis::CleanBaseType(base));
        if (std::find(frontier.begin(), frontier.end(), baseName) != frontier.end())
        {
            return true;
        }
    }
    if (sym.type == SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(sym.signature))
    {
        for (const auto& m : sym.GetClass().includedMixins)
        {
            const std::string mName = analysis::LastScopeSegment(m);
            if (std::find(frontier.begin(), frontier.end(), mName) != frontier.end())
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Linear table scan fallback to collect subtypes.
 * @param[in] rootType Root type name.
 * @param[in] table Symbol table.
 * @return List of subtype symbols.
 */
std::vector<Symbol> CollectSubtypesByScan(const std::string& rootType, const SymbolTable& table)
{
    std::vector<std::string> frontier{analysis::LastScopeSegment(rootType)};
    std::vector<std::string> seen{frontier.front()};
    std::vector<Symbol> subtypes;

    while (!frontier.empty())
    {
        std::vector<std::string> next;

        table.ForEachSymbol(
            [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
            {
                for (const auto& sym : symbols)
                {
                    if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
                    {
                        continue;
                    }

                    const std::string bare = analysis::LastScopeSegment(sym.name);
                    if (std::find(seen.begin(), seen.end(), bare) != seen.end())
                    {
                        continue;
                    }

                    if (MatchesDeclaredBasesOrMixins(sym, frontier))
                    {
                        seen.push_back(bare);
                        next.push_back(bare);
                        subtypes.push_back(sym);
                    }
                }
            });

        frontier = std::move(next);
    }
    return subtypes;
}

/**
 * @brief Collects every type that reaches the given one through its declared bases.
 * @param[in] rootType Base type name.
 * @param[in] table Symbol table.
 * @return List of subtype symbols.
 */
std::vector<Symbol> CollectSubtypes(const std::string& rootType, const SymbolTable& table)
{
    const auto ruleIndex = table.GetRuleIndex();
    if (ruleIndex)
    {
        return CollectSubtypesFromRuleIndex(rootType, table, *ruleIndex);
    }
    return CollectSubtypesByScan(rootType, table);
}

lsp::Location ToLocation(const Symbol& sym)
{
    uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine
                                                                                          : sym.startLine;
    uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0)
                      ? sym.selectionRange.startCharacter
                      : sym.startCharacter;
    uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine
                                                                                          : sym.endLine;
    uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0)
                      ? sym.selectionRange.endCharacter
                      : sym.endCharacter;

    return lsp::Location{lsp::DocumentUri::parse(sym.fileUri),
                         lsp::Range{lsp::Position{sL, sC}, lsp::Position{eL, eC}}};
}

/** @brief The type whose body the cursor sits in, or empty when it sits in none. */
std::string EnclosingType(TSNode node, std::string_view sourceCode)
{
    for (const auto& container : analysis::GetEnclosingContainers(node, sourceCode))
    {
        if (container.kind == analysis::ContainerKind::Class || container.kind == analysis::ContainerKind::Interface)
        {
            return container.qualifiedName.empty() ? container.name : container.qualifiedName;
        }
    }
    return "";
}

/** @brief True when a name is declared as a class or an interface anywhere in the table. */
bool IsTypeName(const std::string& name, const SymbolTable& table)
{
    const auto symbols = table.FindSymbolsPtr(name);
    return symbols && std::any_of(symbols->begin(), symbols->end(), [](const Symbol& sym)
                                  { return sym.type == SymbolType::Class || sym.type == SymbolType::Interface; });
}

/**
 * @brief Resolves implementations for a type symbol under cursor.
 * @param[in] name Type name.
 * @param[in] table Symbol table.
 * @return Locations of derived types or fallback definition.
 */
std::optional<std::vector<lsp::Location>> ResolveTypeImplementations(const std::string& name, const SymbolTable& table)
{
    std::vector<lsp::Location> locations;
    for (const auto& sym : CollectSubtypes(name, table))
    {
        locations.push_back(ToLocation(sym));
    }
    if (locations.empty())
    {
        const auto typeSyms = table.FindSymbolsPtr(name);
        if (typeSyms)
        {
            for (const auto& sym : *typeSyms)
            {
                if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
                {
                    locations.push_back(ToLocation(sym));
                }
            }
        }
    }
    return locations.empty() ? std::nullopt : std::optional{locations};
}

/**
 * @brief Resolves owner type for a member expression or enclosing method.
 * @param[in] node AST node.
 * @param[in] sourceCode Document source text.
 * @param[in] uri Document URI.
 * @param[in] table Symbol table.
 * @return Resolved owner type name.
 */
std::string ResolveOwnerType(TSNode node, std::string_view sourceCode, const std::string& uri, const SymbolTable& table)
{
    std::string owner = EnclosingType(node, sourceCode);
    if (!owner.empty())
    {
        return owner;
    }
    TSNode p = ts_node_parent(node);
    if (!ts_node_is_null(p) && std::string_view(ts_node_type(p)) == "member_expression")
    {
        TSNode objNode = parser::GetChildByField(p, parser::fields::Object);
        if (!ts_node_is_null(objNode))
        {
            const std::string objType = analysis::ResolveExpressionType(objNode, nullptr, table, sourceCode, uri);
            owner = analysis::CleanBaseType(objType);
        }
    }
    return owner;
}

/**
 * @brief Searches for a member symbol inside mixins and bases of a class symbol.
 * @param[in] os Class symbol to inspect.
 * @param[in] name Member name.
 * @param[in] table Symbol table.
 * @return Member symbols pointer if found.
 */
std::shared_ptr<const std::vector<Symbol>> FindMemberInMixinsAndBases(const Symbol& os, const std::string& name,
                                                                      const SymbolTable& table)
{
    if (os.type != SymbolType::Class || !std::holds_alternative<analysis::ClassSignature>(os.signature))
    {
        return nullptr;
    }

    const auto& cls = os.GetClass();
    for (const auto& m : cls.includedMixins)
    {
        auto mSyms = table.FindSymbolsPtr(m + "::" + name);
        if (!mSyms || mSyms->empty())
        {
            mSyms = table.FindSymbolsPtr(analysis::LastScopeSegment(m) + "::" + name);
        }
        if (mSyms && !mSyms->empty())
        {
            return mSyms;
        }
    }

    for (const auto& b : cls.bases)
    {
        const std::string cleanB = analysis::CleanBaseType(b);
        auto mSyms = table.FindSymbolsPtr(cleanB + "::" + name);
        if (!mSyms || mSyms->empty())
        {
            mSyms = table.FindSymbolsPtr(analysis::LastScopeSegment(cleanB) + "::" + name);
        }
        if (mSyms && !mSyms->empty())
        {
            return mSyms;
        }
    }
    return nullptr;
}

/**
 * @brief Finds a member directly declared in a class or its bare alias.
 * @param[in] clsName Qualified class name.
 * @param[in] name Member name.
 * @param[in] table Symbol table.
 * @return Member symbols pointer if found.
 */
std::shared_ptr<const std::vector<Symbol>> FindMemberInClass(const std::string& clsName, const std::string& name,
                                                             const SymbolTable& table)
{
    auto memberSyms = table.FindSymbolsPtr(clsName + "::" + name);
    if (memberSyms && !memberSyms->empty())
    {
        return memberSyms;
    }
    const std::string bareCls = analysis::LastScopeSegment(clsName);
    if (bareCls != clsName)
    {
        memberSyms = table.FindSymbolsPtr(bareCls + "::" + name);
        if (memberSyms && !memberSyms->empty())
        {
            return memberSyms;
        }
    }
    return nullptr;
}

/**
 * @brief Finds a member inside the mixins or bases of an owner class.
 * @param[in] clsName Qualified class name.
 * @param[in] name Member name.
 * @param[in] table Symbol table.
 * @return Member symbols pointer if found.
 */
std::shared_ptr<const std::vector<Symbol>>
FindMemberInOwnerMixinsAndBases(const std::string& clsName, const std::string& name, const SymbolTable& table)
{
    const std::string bareCls = analysis::LastScopeSegment(clsName);
    auto ownerSyms = table.FindSymbolsPtr(clsName);
    if (!ownerSyms || ownerSyms->empty())
    {
        ownerSyms = table.FindSymbolsPtr(bareCls);
    }
    if (!ownerSyms)
    {
        return nullptr;
    }
    for (const auto& os : *ownerSyms)
    {
        auto found = FindMemberInMixinsAndBases(os, name, table);
        if (found && !found->empty())
        {
            return found;
        }
    }
    return nullptr;
}

/**
 * @brief Searches for a member declaration within the hierarchy of an owner type.
 * @param[in] owner Owner type name.
 * @param[in] name Member name.
 * @param[in] table Symbol table.
 * @return Member symbols pointer if found.
 */
std::shared_ptr<const std::vector<Symbol>> FindMemberInHierarchy(const std::string& owner, const std::string& name,
                                                                 const SymbolTable& table)
{
    if (auto syms = FindMemberInClass(owner, name, table))
    {
        return syms;
    }

    auto hierarchy = analysis::GetInheritedTypeHierarchy(owner, table);
    if (hierarchy.empty())
    {
        hierarchy.push_back(owner);
        const std::string bareOwner = analysis::LastScopeSegment(owner);
        if (bareOwner != owner)
        {
            hierarchy.push_back(bareOwner);
        }
    }

    for (const auto& clsName : hierarchy)
    {
        if (auto syms = FindMemberInClass(clsName, name, table))
        {
            return syms;
        }
        if (auto syms = FindMemberInOwnerMixinsAndBases(clsName, name, table))
        {
            return syms;
        }
    }
    return nullptr;
}

/**
 * @brief Checks whether a class is a mixin or registered in hostClassesByMixin.
 * @param[in] owner Full owner name.
 * @param[in] bareOwner Bare owner name.
 * @param[in] table Symbol table.
 * @return True if owner represents a mixin class.
 */
bool CheckMixinOwner(const std::string& owner, const std::string& bareOwner, const SymbolTable& table)
{
    if (analysis::IsMixinClass(owner, table) || (bareOwner != owner && analysis::IsMixinClass(bareOwner, table)))
    {
        return true;
    }
    const auto ruleIndex = table.GetRuleIndex();
    return ruleIndex &&
           (ruleIndex->hostClassesByMixin.contains(owner) || ruleIndex->hostClassesByMixin.contains(bareOwner));
}

/**
 * @brief Accumulator for deduplicated LSP locations.
 */
struct LocationCollector
{
    std::vector<lsp::Location> locations;
    ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenLocs;

    /**
     * @brief Adds a symbol location if not already collected.
     * @param[in] sym Symbol whose location should be captured.
     */
    void Add(const Symbol& sym)
    {
        const uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0)
                                ? sym.selectionRange.startLine
                                : sym.startLine;
        const uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0)
                                ? sym.selectionRange.startCharacter
                                : sym.startCharacter;
        const uint64_t key = (static_cast<uint64_t>(sL) << 32) | sC;
        if (seenLocs.insert({sym.fileUri, key}).second)
        {
            locations.push_back(ToLocation(sym));
        }
    }
};

/**
 * @brief Context required to resolve subtype overrides.
 */
struct MemberImplementationContext
{
    const std::string& name;
    bool isMixinOwner;
    const SymbolTable& table;
    LocationCollector& collector;
};

/**
 * @brief Inspects a subtype for explicit overrides or mixin inclusion.
 * @param[in] subtype Subtype symbol.
 * @param[in,out] ctx Resolution context.
 */
void CollectSubtypeOverrides(const Symbol& subtype, MemberImplementationContext& ctx)
{
    const auto members = ctx.table.FindSymbolsPtr(analysis::LastScopeSegment(subtype.name) + "::" + ctx.name);
    bool hasExplicitOverride = false;
    if (members && !members->empty())
    {
        for (const auto& member : *members)
        {
            if (!member.isSynthesized || !ctx.isMixinOwner)
            {
                ctx.collector.Add(member);
                hasExplicitOverride = true;
            }
        }
    }
    if (!hasExplicitOverride && ctx.isMixinOwner)
    {
        ctx.collector.Add(subtype);
    }
}

/**
 * @brief Resolves implementations/overrides of a member method or field.
 * @param[in] owner Enclosing or receiver owner type name.
 * @param[in] name Member name.
 * @param[in] table Symbol table.
 * @return Locations of overrides or definitions.
 */
std::optional<std::vector<lsp::Location>>
ResolveMemberImplementations(const std::string& owner, const std::string& name, const SymbolTable& table)
{
    auto memberSyms = FindMemberInHierarchy(owner, name, table);
    if (!memberSyms || memberSyms->empty())
    {
        return std::nullopt;
    }

    const std::string bareOwner = analysis::LastScopeSegment(owner);
    const bool isMixinOwner = CheckMixinOwner(owner, bareOwner, table);

    LocationCollector collector;
    MemberImplementationContext ctx{name, isMixinOwner, table, collector};

    for (const auto& subtype : CollectSubtypes(owner, table))
    {
        CollectSubtypeOverrides(subtype, ctx);
    }

    if (isMixinOwner || collector.locations.empty())
    {
        for (const auto& member : *memberSyms)
        {
            collector.Add(member);
        }
    }

    if (collector.locations.empty())
    {
        return std::nullopt;
    }
    return collector.locations;
}

/**
 * @brief Resolves implementations for a free function declaration.
 * @param[in] name Function name.
 * @param[in] table Symbol table.
 * @return List of matching function locations.
 */
std::optional<std::vector<lsp::Location>> ResolveFunctionImplementations(const std::string& name,
                                                                         const SymbolTable& table)
{
    const auto globalSyms = table.FindSymbolsPtr(name);
    if (!globalSyms || globalSyms->empty())
    {
        return std::nullopt;
    }
    std::vector<lsp::Location> locations;
    for (const auto& sym : *globalSyms)
    {
        if (sym.type == SymbolType::Function)
        {
            locations.push_back(ToLocation(sym));
        }
    }
    return locations.empty() ? std::nullopt : std::optional{locations};
}
} // namespace

std::optional<std::vector<lsp::Location>> GetImplementations(const ImplementationRequest& request)
{
    TSNode node{};
    const std::string name = IdentifierAt(request, node);
    if (name.empty())
    {
        return std::nullopt;
    }

    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[Implementation] Resolving implementations for '{}' at {}:{} in {}", name,
                                             request.position.line, request.position.character, request.uri));
    }

    const SymbolTable& table = request.symbolTable;

    // 1. The cursor on a type's own name
    if (IsTypeName(name, table))
    {
        return ResolveTypeImplementations(name, table);
    }

    // 2. Member method or field
    const std::string owner = ResolveOwnerType(node, request.sourceCode, request.uri, table);
    if (!owner.empty())
    {
        auto result = ResolveMemberImplementations(owner, name, table);
        if (result.has_value())
        {
            return result;
        }
    }

    // 3. Free function fallback
    return ResolveFunctionImplementations(name, table);
}
} // namespace angel_lsp::features
