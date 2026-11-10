#include "analysis/checkers/AccessCheckerCommon.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "config/EngineRuleConfig.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/** @brief Compares two type names by their last segment, so a qualification cannot hide a match. */
bool IsSameAccessType(std::string_view a, std::string_view b)
{
    return !a.empty() && (a == b || LastScopeSegment(a) == LastScopeSegment(b));
}

/** @brief The access modifier a declaration carries, whatever kind of declaration it is. */
static bool TryReadAccess(const Symbol& sym, AccessModifier& access)
{
    if (std::holds_alternative<FunctionSignature>(sym.signature))
    {
        access = sym.GetFunction().modifiers.access;
        return true;
    }
    if (std::holds_alternative<VariableSignature>(sym.signature))
    {
        access = sym.GetVariable().modifiers.access;
        return true;
    }
    return false;
}

/**
 * @brief Checks whether property accessors (get_X/set_X) are enabled by engine configuration.
 */
static bool AccessorStandsForProperty(const Symbol& sym, bool keywordRequired)
{
    if (!keywordRequired)
    {
        return true;
    }
    return std::holds_alternative<FunctionSignature>(sym.signature) && sym.GetFunction().modifiers.isProperty;
}

/**
 * @brief Collects symbols matching the member name directly on the specified owner type.
 *
 * @param[in] owner Type name owning the candidate member.
 * @param[in] memberName Member name to search.
 * @param[in] ctx Lookup context bundling type hierarchy and symbol table.
 * @param[out] viaAccessor Set to true if match was via getter/setter accessor.
 * @return Vector of matched symbols.
 */
static std::vector<Symbol> CollectDirectCandidates(std::string_view owner, std::string_view memberName,
                                                   const MemberLookupContext& ctx, bool& viaAccessor)
{
    std::vector<Symbol> candidates;
    auto candidatesPtr = ctx.table.FindMemberSymbolPtr(owner, memberName);
    if (candidatesPtr && !candidatesPtr->empty())
    {
        candidates.insert(candidates.end(), candidatesPtr->begin(), candidatesPtr->end());
        const bool hasInstance = std::any_of(candidates.begin(), candidates.end(),
                                             [](const Symbol& s)
                                             {
                                                 return !(s.type == SymbolType::Variable &&
                                                          std::holds_alternative<VariableSignature>(s.signature) &&
                                                          s.GetVariable().isEnumConstant);
                                             });
        if (hasInstance)
        {
            std::stable_partition(candidates.begin(), candidates.end(),
                                  [](const Symbol& s)
                                  {
                                      return !(s.type == SymbolType::Variable &&
                                               std::holds_alternative<VariableSignature>(s.signature) &&
                                               s.GetVariable().isEnumConstant);
                                  });
            return candidates;
        }
    }

    for (const auto* accessorPrefix : {"get_", "set_"})
    {
        std::string accName(accessorPrefix);
        accName.append(memberName);
        auto accSyms = ctx.table.FindMemberSymbolPtr(owner, accName);
        if (!accSyms)
        {
            continue;
        }
        for (const auto& sym : *accSyms)
        {
            if (AccessorStandsForProperty(sym, ctx.accessorKeywordRequired))
            {
                viaAccessor = true;
                candidates.push_back(sym);
            }
        }
    }
    std::stable_partition(candidates.begin(), candidates.end(),
                          [](const Symbol& s)
                          {
                              return !(s.type == SymbolType::Variable &&
                                       std::holds_alternative<VariableSignature>(s.signature) &&
                                       s.GetVariable().isEnumConstant);
                          });
    return candidates;
}

/**
 * @brief Collects symbols matching the member name via container matching on the specified owner type.
 *
 * @param[in] owner Type name owning the candidate member.
 * @param[in] memberName Member name to search.
 * @param[in] ctx Lookup context bundling type hierarchy and symbol table.
 * @param[out] viaAccessor Set to true if match was via getter/setter accessor.
 * @return Vector of matched symbols.
 */
static std::vector<Symbol> CollectContainerCandidates(std::string_view owner, std::string_view memberName,
                                                      const MemberLookupContext& ctx, bool& viaAccessor)
{
    std::vector<Symbol> candidates;
    if (auto memberSyms = ctx.table.FindSymbolsPtr(memberName))
    {
        for (const auto& sym : *memberSyms)
        {
            if (IsSameAccessType(sym.containerName, owner))
            {
                candidates.push_back(sym);
            }
        }
    }
    if (!candidates.empty())
    {
        return candidates;
    }

    for (const auto* accessorPrefix : {"get_", "set_"})
    {
        std::string accName(accessorPrefix);
        accName.append(memberName);
        if (auto syms = ctx.table.FindSymbolsPtr(accName))
        {
            for (const auto& sym : *syms)
            {
                if (IsSameAccessType(sym.containerName, owner) &&
                    AccessorStandsForProperty(sym, ctx.accessorKeywordRequired))
                {
                    viaAccessor = true;
                    candidates.push_back(sym);
                }
            }
        }
    }
    return candidates;
}

/**
 * @brief Collects all candidate symbols for a member name on an owner type.
 *
 * @param[in] owner Type name owning the candidate member.
 * @param[in] memberName Member name to search.
 * @param[in] ctx Lookup context bundling type hierarchy and symbol table.
 * @param[out] viaAccessor Set to true if match was via getter/setter accessor.
 * @return Vector of candidate symbols.
 */
static std::vector<Symbol> CollectOwnerCandidates(std::string_view owner, std::string_view memberName,
                                                  const MemberLookupContext& ctx, bool& viaAccessor)
{
    auto candidates = CollectDirectCandidates(owner, memberName, ctx, viaAccessor);
    if (candidates.empty())
    {
        candidates = CollectContainerCandidates(owner, memberName, ctx, viaAccessor);
    }
    return candidates;
}

/**
 * @brief Evaluates access for candidate symbols, updating the resolved MemberLookupResult result.
 *
 * @param[in] candidates Candidate symbols found for the owner.
 * @param[in] owner Owner type name.
 * @param[in] ctx Lookup context bundling type hierarchy and symbol table.
 * @param[in,out] result Member access state to update.
 * @return True if resolution concluded (public access reached).
 */
static bool ApplyCandidatesAccess(const std::vector<Symbol>& candidates, std::string_view owner,
                                  const MemberLookupContext& ctx, MemberLookupResult& result)
{
    for (const auto& sym : candidates)
    {
        AccessModifier access = AccessModifier::Public;
        if (!TryReadAccess(sym, access))
        {
            continue;
        }

        result.found = true;

        if (access == AccessModifier::Public)
        {
            result.decided = false;
            result.access = AccessModifier::Public;
            result.declaringClass = IsMixinClass(owner, ctx.table) ? ctx.typeName : std::string(owner);
            return true;
        }

        if (!result.decided || (result.access == AccessModifier::Private && access == AccessModifier::Protected))
        {
            result.decided = true;
            result.access = access;
            result.declaringClass = IsMixinClass(owner, ctx.table) ? ctx.typeName : std::string(owner);
        }
    }
    return false;
}

MemberLookupResult FindMember(std::string_view typeName, std::string_view memberName, const SymbolTable& table,
                              bool accessorKeywordRequired)
{
    MemberLookupResult result;
    if (typeName.empty() || memberName.empty())
    {
        return result;
    }

    const auto ruleIndex = table.GetRuleIndex();
    const MemberLookupContext ctx{std::string(typeName), table, accessorKeywordRequired};
    for (const auto& owner : GetInheritedTypeHierarchy(typeName, table))
    {
        if (ruleIndex)
        {
            const auto& members = ruleIndex->Members(owner);
            std::string getAcc("get_");
            getAcc.append(memberName);
            std::string setAcc("set_");
            setAcc.append(memberName);
            const bool hasPotential = members.allMemberNames.contains(memberName) ||
                                      members.allMemberNames.contains(getAcc) ||
                                      members.allMemberNames.contains(setAcc);
            if (!hasPotential)
            {
                continue;
            }
        }
        auto candidates = CollectOwnerCandidates(owner, memberName, ctx, result.viaAccessor);
        if (ApplyCandidatesAccess(candidates, owner, ctx, result))
        {
            return result;
        }
    }

    return result;
}

MemberLookupResult FindMemberWithProxies(std::string_view objectType, std::string_view memberName,
                                         const SymbolTable& table, const DiagnosticContext& ctx)
{
    MemberLookupResult member = FindMember(objectType, memberName, table, ctx.request.RequiresAccessorKeyword());
    if (member.found || !ctx.request.engineRules)
    {
        return member;
    }

    const auto& proxyMap = ctx.request.engineRules->unusedRules.baseClassProxyMap;
    if (proxyMap.empty())
    {
        return member;
    }

    auto hier = GetInheritedTypeHierarchy(objectType, table);
    for (const auto& owner : hier)
    {
        auto it = proxyMap.find(std::string(owner));
        if (it != proxyMap.end())
        {
            for (const auto& proxy : it->second)
            {
                member = FindMember(proxy, memberName, table, ctx.request.RequiresAccessorKeyword());
                if (member.found)
                {
                    return member;
                }
            }
        }
    }
    return member;
}

} // namespace angel_lsp::analysis
