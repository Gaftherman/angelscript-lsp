#include "features/implementation/ImplementationHandler.h"

#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/LspLogger.h"

#include <algorithm>
#include <memory>
#include <spdlog/fmt/fmt.h>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Expands derived types from a list of records and records matches in the accumulator.
 * @param[in] derivedList List of derived type records.
 * @param[in] table analysis::Symbol table.
 * @param[in,out] acc Subtype accumulator.
 */
template <typename TList>
static void ExpandDerivedFromList(const TList& derivedList, const analysis::SymbolTable& table, SubtypeAccumulator& acc)
{
    for (const auto& derived : derivedList)
    {
        const std::string bare{analysis::LastScopeSegment(derived.name)};
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
            for (const analysis::Symbol& s : *symList)
            {
                if (s.type == analysis::SymbolType::Class || s.type == analysis::SymbolType::Interface)
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
 * @param[in] table analysis::Symbol table.
 * @param[in] ruleIndex Precomputed rule index.
 * @return List of subtype symbols.
 */
static std::vector<analysis::Symbol> CollectSubtypesFromRuleIndex(const std::string& rootType,
                                                                  const analysis::SymbolTable& table,
                                                                  const analysis::rules::RuleIndex& ruleIndex)
{
    const std::string bareRoot{analysis::LastScopeSegment(rootType)};
    std::vector<std::string> frontier{bareRoot};
    if (bareRoot != rootType && !rootType.empty())
    {
        frontier.push_back(rootType);
    }
    ankerl::unordered_dense::set<std::string> seen{bareRoot, rootType};
    std::vector<analysis::Symbol> subtypes;

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
static bool MatchesDeclaredBasesOrMixins(const analysis::Symbol& sym, const std::vector<std::string>& frontier)
{
    for (const auto& base : analysis::GetDeclaredBases(sym))
    {
        const std::string baseName{analysis::LastScopeSegment(analysis::CleanBaseType(base))};
        if (std::find(frontier.begin(), frontier.end(), baseName) != frontier.end())
        {
            return true;
        }
    }
    if (sym.type == analysis::SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(sym.signature))
    {
        for (const auto& m : sym.GetClass().includedMixins)
        {
            const std::string mName{analysis::LastScopeSegment(m)};
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
 * @param[in] table analysis::Symbol table.
 * @return List of subtype symbols.
 */
static std::vector<analysis::Symbol> CollectSubtypesByScan(const std::string& rootType,
                                                           const analysis::SymbolTable& table)
{
    std::vector<std::string> frontier{std::string(analysis::LastScopeSegment(rootType))};
    std::vector<std::string> seen{frontier.front()};
    std::vector<analysis::Symbol> subtypes;

    while (!frontier.empty())
    {
        std::vector<std::string> next;

        table.ForEachSymbol(
            [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
            {
                for (const auto& sym : symbols)
                {
                    if (sym.type != analysis::SymbolType::Class && sym.type != analysis::SymbolType::Interface)
                    {
                        continue;
                    }

                    const std::string bare{analysis::LastScopeSegment(sym.name)};
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
 * @param[in] table analysis::Symbol table.
 * @return List of subtype symbols.
 */
static std::vector<analysis::Symbol> CollectSubtypes(const std::string& rootType, const analysis::SymbolTable& table)
{
    const auto ruleIndex = table.GetRuleIndex();
    if (ruleIndex)
    {
        return CollectSubtypesFromRuleIndex(rootType, table, *ruleIndex);
    }
    return CollectSubtypesByScan(rootType, table);
}

static lsp::Location ToLocation(const analysis::Symbol& sym)
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

void LocationCollector::Add(const analysis::Symbol& sym)
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

/**
 * @brief True when a name is declared as a class or an interface anywhere in the table.
 */
static bool IsTypeName(const std::string& name, const analysis::SymbolTable& table)
{
    const auto symbols = table.FindSymbolsPtr(name);
    if (symbols &&
        std::any_of(symbols->begin(), symbols->end(), [](const analysis::Symbol& sym)
                    { return sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface; }))
    {
        return true;
    }
    const auto shortSymbols = table.FindTypeSymbolsByShortName(analysis::LastScopeSegment(name));
    return std::any_of(
        shortSymbols.begin(), shortSymbols.end(), [](const analysis::Symbol& sym)
        { return sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface; });
}

/**
 * @brief Resolves implementations for a type symbol under cursor.
 * @param[in] name Type name.
 * @param[in] table analysis::Symbol table.
 * @return Locations of derived types or fallback definition.
 */
static std::optional<std::vector<lsp::Location>> ResolveTypeImplementations(const std::string& name,
                                                                            const analysis::SymbolTable& table)
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
                if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface)
                {
                    locations.push_back(ToLocation(sym));
                }
            }
        }
        if (locations.empty())
        {
            for (const auto& sym : table.FindTypeSymbolsByShortName(analysis::LastScopeSegment(name)))
            {
                if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface)
                {
                    locations.push_back(ToLocation(sym));
                }
            }
        }
    }
    return locations.empty() ? std::nullopt : std::optional{locations};
}

/**
 * @brief Finds a member directly declared in a class or its bare alias.
 */
static std::shared_ptr<const std::vector<analysis::Symbol>>
FindMemberInClass(const std::string& clsName, const std::string& name, const analysis::SymbolTable& table)
{
    auto memberSyms = table.FindMemberSymbolPtr(clsName, name);
    if (memberSyms && !memberSyms->empty())
    {
        return memberSyms;
    }
    const std::string bareCls{analysis::LastScopeSegment(clsName)};
    if (bareCls != clsName)
    {
        memberSyms = table.FindMemberSymbolPtr(bareCls, name);
        if (memberSyms && !memberSyms->empty())
        {
            return memberSyms;
        }
    }
    return nullptr;
}

/**
 * @brief Searches for a member symbol inside mixins and bases of a class symbol.
 */
static std::shared_ptr<const std::vector<analysis::Symbol>>
FindMemberInMixinsAndBases(const analysis::Symbol& os, const std::string& name, const analysis::SymbolTable& table)
{
    if (os.type != analysis::SymbolType::Class || !std::holds_alternative<analysis::ClassSignature>(os.signature))
    {
        return nullptr;
    }

    const auto& cls = os.GetClass();
    for (const auto& m : cls.includedMixins)
    {
        if (auto mSyms = FindMemberInClass(m, name, table); mSyms && !mSyms->empty())
        {
            return mSyms;
        }
    }

    for (const auto& b : cls.bases)
    {
        const std::string cleanB = analysis::CleanBaseType(b);
        if (auto mSyms = FindMemberInClass(cleanB, name, table); mSyms && !mSyms->empty())
        {
            return mSyms;
        }
    }
    return nullptr;
}

/**
 * @brief Finds a member inside the mixins or bases of an owner class.
 */
static std::shared_ptr<const std::vector<analysis::Symbol>>
FindMemberInOwnerMixinsAndBases(const std::string& clsName, const std::string& name, const analysis::SymbolTable& table)
{
    const std::string bareCls{analysis::LastScopeSegment(clsName)};
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
 */
static std::shared_ptr<const std::vector<analysis::Symbol>>
FindMemberInHierarchy(const std::string& owner, const std::string& name, const analysis::SymbolTable& table)
{
    if (auto syms = FindMemberInClass(owner, name, table))
    {
        return syms;
    }

    auto hierarchy = analysis::GetInheritedTypeHierarchy(owner, table);
    if (hierarchy.empty())
    {
        hierarchy.push_back(owner);
        const std::string bareOwner{analysis::LastScopeSegment(owner)};
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
 */
static bool CheckMixinOwner(const std::string& owner, const std::string& bareOwner, const analysis::SymbolTable& table)
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
 * @brief Inspects a subtype for explicit overrides or mixin inclusion.
 */
static void CollectSubtypeOverrides(const analysis::Symbol& subtype, MemberImplementationContext& ctx)
{
    const auto members = ctx.table.FindMemberSymbolPtr(analysis::LastScopeSegment(subtype.name), ctx.name);
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
 */
static std::optional<std::vector<lsp::Location>>
ResolveMemberImplementations(const std::string& owner, const std::string& name, const analysis::SymbolTable& table)
{
    auto memberSyms = FindMemberInHierarchy(owner, name, table);
    if (!memberSyms || memberSyms->empty())
    {
        return std::nullopt;
    }

    const std::string bareOwner{analysis::LastScopeSegment(owner)};
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
 */
static std::optional<std::vector<lsp::Location>> ResolveFunctionImplementations(const std::string& name,
                                                                                const analysis::SymbolTable& table)
{
    const auto globalSyms = table.FindSymbolsPtr(name);
    if (!globalSyms || globalSyms->empty())
    {
        return std::nullopt;
    }
    std::vector<lsp::Location> locations;
    for (const auto& sym : *globalSyms)
    {
        if (sym.type == analysis::SymbolType::Function)
        {
            locations.push_back(ToLocation(sym));
        }
    }
    return locations.empty() ? std::nullopt : std::optional{locations};
}

std::optional<std::vector<lsp::Location>> GetImplementations(const ImplementationRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    std::string name = common::FindScopedIdentifierAtPosition(request.sourceCode, request.position);
    if (name.empty())
    {
        name = common::FindIdentifierAtPosition(request.sourceCode, request.position);
    }
    if (name.empty())
    {
        return std::nullopt;
    }

    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[Implementation] Resolving implementations for '{}' at {}:{} in {}", name,
                                             request.position.line, request.position.character, request.uri));
    }

    const analysis::SymbolTable& table = request.symbolTable;

    // Type name
    if (IsTypeName(name, table))
    {
        return ResolveTypeImplementations(name, table);
    }

    // Member method or field
    const std::string owner = table.FindEnclosingClassName(request.uri, request.position.line);
    if (!owner.empty())
    {
        auto result = ResolveMemberImplementations(owner, name, table);
        if (result.has_value())
        {
            return result;
        }
    }

    // Free function fallback
    return ResolveFunctionImplementations(name, table);
}
} // namespace angel_lsp::features
