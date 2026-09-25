#include "analysis/rules/RuleIndex.h"
#include "analysis/SemanticHelpers.h"
#include <algorithm>

namespace angel_lsp::analysis::rules
{
const ContainerMembers& RuleIndex::Members(std::string_view containerName) const
{
    static const ContainerMembers empty;
    const auto it = byContainer.find(containerName);
    return it == byContainer.end() ? empty : it->second;
}

const ContainerMembers& RuleIndex::Members(const std::string& containerName) const
{
    return Members(std::string_view(containerName));
}

void RuleIndexPartial::Merge(RuleIndexPartial&& other)
{
    allNames.insert(allNames.end(), std::make_move_iterator(other.allNames.begin()),
                    std::make_move_iterator(other.allNames.end()));

    enumMembers.insert(enumMembers.end(), std::make_move_iterator(other.enumMembers.begin()),
                       std::make_move_iterator(other.enumMembers.end()));

    qualifiedTypes.insert(qualifiedTypes.end(), std::make_move_iterator(other.qualifiedTypes.begin()),
                          std::make_move_iterator(other.qualifiedTypes.end()));

    globalAccessorProperties.insert(globalAccessorProperties.end(),
                                    std::make_move_iterator(other.globalAccessorProperties.begin()),
                                    std::make_move_iterator(other.globalAccessorProperties.end()));

    keywordGlobalAccessorProperties.insert(keywordGlobalAccessorProperties.end(),
                                           std::make_move_iterator(other.keywordGlobalAccessorProperties.begin()),
                                           std::make_move_iterator(other.keywordGlobalAccessorProperties.end()));

    derivedByBase.insert(derivedByBase.end(), std::make_move_iterator(other.derivedByBase.begin()),
                         std::make_move_iterator(other.derivedByBase.end()));

    hostClassesByMixin.insert(hostClassesByMixin.end(), std::make_move_iterator(other.hostClassesByMixin.begin()),
                              std::make_move_iterator(other.hostClassesByMixin.end()));

    for (auto& [containerName, contrib] : other.byContainer)
    {
        auto& target = byContainer[containerName];
        target.methodNames.insert(target.methodNames.end(), std::make_move_iterator(contrib.methodNames.begin()),
                                  std::make_move_iterator(contrib.methodNames.end()));
        target.finalMethodNames.insert(target.finalMethodNames.end(),
                                       std::make_move_iterator(contrib.finalMethodNames.begin()),
                                       std::make_move_iterator(contrib.finalMethodNames.end()));
        target.allMemberNames.insert(target.allMemberNames.end(),
                                     std::make_move_iterator(contrib.allMemberNames.begin()),
                                     std::make_move_iterator(contrib.allMemberNames.end()));
        target.memberKeys.insert(target.memberKeys.end(), std::make_move_iterator(contrib.memberKeys.begin()),
                                 std::make_move_iterator(contrib.memberKeys.end()));
        target.accessorProperties.insert(target.accessorProperties.end(),
                                         std::make_move_iterator(contrib.accessorProperties.begin()),
                                         std::make_move_iterator(contrib.accessorProperties.end()));
        target.keywordAccessorProperties.insert(target.keywordAccessorProperties.end(),
                                                std::make_move_iterator(contrib.keywordAccessorProperties.begin()),
                                                std::make_move_iterator(contrib.keywordAccessorProperties.end()));
        target.nestedTypeCount += contrib.nestedTypeCount;
    }
}

namespace
{
/** @brief Extracts and records template parameters and namespace segments. */
void ProcessSymbolNames(RuleIndexPartial& partial, const Symbol& sym)
{
    partial.allNames.push_back(sym.name);

    if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
    {
        for (const auto& param : sym.GetClass().templateParams)
        {
            partial.allNames.push_back(param);
        }
    }

    if (sym.type == SymbolType::Namespace)
    {
        std::string_view remaining = sym.name;
        for (size_t at = remaining.find("::"); at != std::string_view::npos; at = remaining.find("::"))
        {
            partial.allNames.push_back(std::string(remaining.substr(0, at)));
            remaining.remove_prefix(at + 2);
        }
        if (!remaining.empty())
        {
            partial.allNames.push_back(std::string(remaining));
        }
    }
}

/** @brief Records enum members. */
void ProcessEnumSymbol(RuleIndexPartial& partial, const Symbol& sym)
{
    if (sym.type == SymbolType::Enum && std::holds_alternative<EnumSignature>(sym.signature))
    {
        for (const auto& member : sym.GetEnum().members)
        {
            partial.enumMembers.emplace_back(member.name, sym);
        }
    }
}

/** @brief Records qualified type names for types. */
void ProcessTypeSymbol(RuleIndexPartial& partial, const Symbol& sym)
{
    if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface || sym.type == SymbolType::Enum ||
        sym.type == SymbolType::Typedef || sym.type == SymbolType::Funcdef)
    {
        const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
        partial.qualifiedTypes.emplace_back(sym.name, qName);
    }
}

/** @brief Helper to record a base type in derivedByBase. */
void RecordBase(RuleIndexPartial& partial, const DerivedType& derived, const std::string& base)
{
    const std::string cleanBase = CleanBaseType(base);
    if (!cleanBase.empty())
    {
        partial.derivedByBase.emplace_back(cleanBase, derived);
    }
    if (!base.empty() && base != cleanBase)
    {
        partial.derivedByBase.emplace_back(base, derived);
    }
}

/** @brief Records inheritance and mixin relationships. */
void ProcessInheritanceSymbol(RuleIndexPartial& partial, const Symbol& sym)
{
    if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
    {
        return;
    }

    const DerivedType derived{sym.qualifiedName.empty() ? sym.name : sym.qualifiedName, sym.name};

    if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
    {
        const auto& classSig = sym.GetClass();
        for (const auto& base : classSig.bases)
        {
            RecordBase(partial, derived, base);
        }

        for (const auto& mixinName : classSig.includedMixins)
        {
            const std::string cleanMixin = CleanBaseType(mixinName);
            if (!cleanMixin.empty())
            {
                partial.hostClassesByMixin.emplace_back(cleanMixin, derived);
                const std::string bareMixin = LastScopeSegment(cleanMixin);
                if (bareMixin != cleanMixin)
                {
                    partial.hostClassesByMixin.emplace_back(bareMixin, derived);
                }
            }
        }
    }
    else if (sym.type == SymbolType::Interface && std::holds_alternative<InterfaceSignature>(sym.signature))
    {
        for (const auto& base : sym.GetInterface().inheritedInterfaces)
        {
            RecordBase(partial, derived, base);
        }
    }
}

/** @brief Records accessor property names from getter/setter functions. */
void RecordAccessorProperty(RuleIndexPartial& partial, const Symbol& sym)
{
    std::string_view accessor = sym.name;
    if (accessor.starts_with("get_") || accessor.starts_with("set_"))
    {
        accessor.remove_prefix(4);
        if (!accessor.empty())
        {
            const bool isProp =
                std::holds_alternative<FunctionSignature>(sym.signature) && sym.GetFunction().modifiers.isProperty;
            if (sym.containerName.empty())
            {
                partial.globalAccessorProperties.emplace_back(accessor);
                if (isProp)
                {
                    partial.keywordGlobalAccessorProperties.emplace_back(accessor);
                }
            }
            else
            {
                auto& target = partial.byContainer[sym.containerName];
                target.accessorProperties.emplace_back(accessor);
                if (isProp)
                {
                    target.keywordAccessorProperties.emplace_back(accessor);
                }
            }
        }
    }
}

/** @brief Records container members and their nested types. */
void ProcessContainerMember(RuleIndexPartial& partial, const Symbol& sym)
{
    if (sym.containerName.empty())
    {
        return;
    }

    auto& members = partial.byContainer[sym.containerName];
    members.allMemberNames.push_back(sym.name);
    members.memberKeys.push_back(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName);

    switch (sym.type)
    {
    case SymbolType::Function:
        members.methodNames.push_back(sym.name);
        if (std::holds_alternative<FunctionSignature>(sym.signature) && sym.GetFunction().modifiers.isFinal)
        {
            members.finalMethodNames.push_back(sym.name);
        }
        RecordAccessorProperty(partial, sym);
        break;
    case SymbolType::Class:
    case SymbolType::Interface:
    case SymbolType::Enum:
    case SymbolType::Typedef:
    case SymbolType::Funcdef:
        members.nestedTypeCount++;
        break;
    default:
        break;
    }
}

/** @brief Processes a single symbol and extracts its contributions to the partial index. */
void ProcessSymbol(RuleIndexPartial& partial, const Symbol& sym)
{
    ProcessSymbolNames(partial, sym);
    ProcessEnumSymbol(partial, sym);
    ProcessTypeSymbol(partial, sym);
    ProcessInheritanceSymbol(partial, sym);

    if (sym.containerName.empty() && sym.type == SymbolType::Function)
    {
        RecordAccessorProperty(partial, sym);
    }

    ProcessContainerMember(partial, sym);
}

/** @brief Populates partial index from range of symbols. */
template <typename SymbolRange> void PopulatePartial(RuleIndexPartial& partial, const SymbolRange& symbols)
{
    for (const auto& symRef : symbols)
    {
        const Symbol& sym = [&]() -> const Symbol&
        {
            if constexpr (std::is_pointer_v<std::decay_t<decltype(symRef)>>)
            {
                return *symRef;
            }
            else
            {
                return symRef;
            }
        }();

        if (sym.isSynthesized)
        {
            continue;
        }

        ProcessSymbol(partial, sym);
    }
}

/** @brief Applies symbol names and enum members from partial index. */
void ApplyNamesAndEnums(RuleIndex& index, const RuleIndexPartial& partial)
{
    for (const auto& name : partial.allNames)
    {
        ++index.allNames[name];
    }

    for (const auto& [name, sym] : partial.enumMembers)
    {
        if (++index.enumMemberCounts[name] == 1)
        {
            index.enumMemberNames.insert(name);
        }
        index.enumSymbolsByMemberName[name].push_back(sym);
    }
}

/** @brief Applies qualified types and property accessors from partial index. */
void ApplyTypesAndAccessors(RuleIndex& index, const RuleIndexPartial& partial)
{
    for (const auto& [shortName, qName] : partial.qualifiedTypes)
    {
        auto& counts = index.qualifiedTypeCounts[shortName];
        if (++counts[qName] == 1)
        {
            index.qualifiedTypesByShortName[shortName].push_back(qName);
        }
    }

    for (const auto& p : partial.globalAccessorProperties)
    {
        if (++index.globalAccessorPropertyCounts[p] == 1)
        {
            index.globalAccessorPropertyNames.insert(p);
        }
    }

    for (const auto& p : partial.keywordGlobalAccessorProperties)
    {
        if (++index.keywordGlobalAccessorPropertyCounts[p] == 1)
        {
            index.keywordGlobalAccessorPropertyNames.insert(p);
        }
    }
}

/** @brief Applies inheritance and mixin relations from partial index. */
void ApplyHierarchy(RuleIndex& index, const RuleIndexPartial& partial)
{
    for (const auto& [base, derived] : partial.derivedByBase)
    {
        index.derivedByBase[base].push_back(derived);
    }

    for (const auto& [mixin, host] : partial.hostClassesByMixin)
    {
        index.hostClassesByMixin[mixin].push_back(host);
    }
}

/** @brief Applies contributions for a single container. */
void ApplyContainerContribution(ContainerMembers& cm, const RuleIndexPartial::ContainerContribution& contrib)
{
    for (const auto& m : contrib.methodNames)
    {
        if (++cm.methodCounts[m] == 1)
        {
            cm.methodNames.insert(m);
        }
    }
    for (const auto& m : contrib.finalMethodNames)
    {
        if (++cm.finalMethodCounts[m] == 1)
        {
            cm.finalMethodNames.insert(m);
        }
    }
    for (const auto& m : contrib.allMemberNames)
    {
        if (++cm.allMemberCounts[m] == 1)
        {
            cm.allMemberNames.insert(m);
        }
    }
    for (const auto& p : contrib.accessorProperties)
    {
        if (++cm.accessorPropertyCounts[p] == 1)
        {
            cm.accessorPropertyNames.insert(p);
        }
    }
    for (const auto& p : contrib.keywordAccessorProperties)
    {
        if (++cm.keywordAccessorPropertyCounts[p] == 1)
        {
            cm.keywordAccessorPropertyNames.insert(p);
        }
    }
    for (const auto& k : contrib.memberKeys)
    {
        cm.memberKeys.push_back(k);
        if (++cm.memberKeyCounts[k] == 1)
        {
            cm.memberKeySet.insert(k);
        }
    }
    cm.nestedTypeCount += contrib.nestedTypeCount;
    cm.hasNestedType = (cm.nestedTypeCount > 0);
}

/** @brief Applies container contributions from partial index. */
void ApplyContainers(RuleIndex& index, const RuleIndexPartial& partial)
{
    for (const auto& [containerName, contrib] : partial.byContainer)
    {
        ApplyContainerContribution(index.byContainer[containerName], contrib);
    }
}

/** @brief Removes symbol names and enum members of a partial index. */
void RemoveNamesAndEnums(RuleIndex& index, const RuleIndexPartial& partial)
{
    for (const auto& name : partial.allNames)
    {
        auto it = index.allNames.find(name);
        if (it != index.allNames.end() && --it->second == 0)
        {
            index.allNames.erase(it);
        }
    }

    for (const auto& [name, sym] : partial.enumMembers)
    {
        auto it = index.enumMemberCounts.find(name);
        if (it != index.enumMemberCounts.end() && --it->second == 0)
        {
            index.enumMemberCounts.erase(it);
            index.enumMemberNames.erase(name);
        }
        auto sIt = index.enumSymbolsByMemberName.find(name);
        if (sIt != index.enumSymbolsByMemberName.end())
        {
            std::erase_if(sIt->second,
                          [&](const Symbol& s) { return s.fileUri == partial.fileUri && s.name == sym.name; });
            if (sIt->second.empty())
            {
                index.enumSymbolsByMemberName.erase(sIt);
            }
        }
    }
}

/** @brief Removes qualified types and property accessors of a partial index. */
void RemoveTypesAndAccessors(RuleIndex& index, const RuleIndexPartial& partial)
{
    for (const auto& [shortName, qName] : partial.qualifiedTypes)
    {
        auto qIt = index.qualifiedTypeCounts.find(shortName);
        if (qIt != index.qualifiedTypeCounts.end())
        {
            auto cIt = qIt->second.find(qName);
            if (cIt != qIt->second.end() && --cIt->second == 0)
            {
                qIt->second.erase(cIt);
                auto vecIt = index.qualifiedTypesByShortName.find(shortName);
                if (vecIt != index.qualifiedTypesByShortName.end())
                {
                    std::erase(vecIt->second, qName);
                    if (vecIt->second.empty())
                    {
                        index.qualifiedTypesByShortName.erase(vecIt);
                    }
                }
            }
            if (qIt->second.empty())
            {
                index.qualifiedTypeCounts.erase(qIt);
            }
        }
    }

    auto removeCountedProp = [](auto& counts, auto& set, const std::string& p)
    {
        auto it = counts.find(p);
        if (it != counts.end() && --it->second == 0)
        {
            counts.erase(it);
            set.erase(p);
        }
    };

    for (const auto& p : partial.globalAccessorProperties)
    {
        removeCountedProp(index.globalAccessorPropertyCounts, index.globalAccessorPropertyNames, p);
    }
    for (const auto& p : partial.keywordGlobalAccessorProperties)
    {
        removeCountedProp(index.keywordGlobalAccessorPropertyCounts, index.keywordGlobalAccessorPropertyNames, p);
    }
}

/** @brief Removes inheritance and mixin relations of a partial index. */
void RemoveHierarchy(RuleIndex& index, const RuleIndexPartial& partial)
{
    for (const auto& [base, derived] : partial.derivedByBase)
    {
        auto it = index.derivedByBase.find(base);
        if (it != index.derivedByBase.end())
        {
            std::erase_if(it->second, [&](const DerivedType& d)
                          { return d.qualifiedName == derived.qualifiedName && d.name == derived.name; });
            if (it->second.empty())
            {
                index.derivedByBase.erase(it);
            }
        }
    }

    for (const auto& [mixin, host] : partial.hostClassesByMixin)
    {
        auto it = index.hostClassesByMixin.find(mixin);
        if (it != index.hostClassesByMixin.end())
        {
            std::erase_if(it->second, [&](const DerivedType& d)
                          { return d.qualifiedName == host.qualifiedName && d.name == host.name; });
            if (it->second.empty())
            {
                index.hostClassesByMixin.erase(it);
            }
        }
    }
}

/** @brief Removes contributions for a single container. */
void RemoveContainerContribution(ContainerMembers& cm, const RuleIndexPartial::ContainerContribution& contrib)
{
    auto removeCountedMember = [](auto& counts, auto& set, const std::string& item)
    {
        auto it = counts.find(item);
        if (it != counts.end() && --it->second == 0)
        {
            counts.erase(it);
            set.erase(item);
        }
    };

    for (const auto& m : contrib.methodNames)
    {
        removeCountedMember(cm.methodCounts, cm.methodNames, m);
    }
    for (const auto& m : contrib.finalMethodNames)
    {
        removeCountedMember(cm.finalMethodCounts, cm.finalMethodNames, m);
    }
    for (const auto& m : contrib.allMemberNames)
    {
        removeCountedMember(cm.allMemberCounts, cm.allMemberNames, m);
    }
    for (const auto& p : contrib.accessorProperties)
    {
        removeCountedMember(cm.accessorPropertyCounts, cm.accessorPropertyNames, p);
    }
    for (const auto& p : contrib.keywordAccessorProperties)
    {
        removeCountedMember(cm.keywordAccessorPropertyCounts, cm.keywordAccessorPropertyNames, p);
    }
    for (const auto& k : contrib.memberKeys)
    {
        auto it = cm.memberKeyCounts.find(k);
        if (it != cm.memberKeyCounts.end() && --it->second == 0)
        {
            cm.memberKeyCounts.erase(it);
            cm.memberKeySet.erase(k);
        }
        auto vecIt = std::find(cm.memberKeys.begin(), cm.memberKeys.end(), k);
        if (vecIt != cm.memberKeys.end())
        {
            cm.memberKeys.erase(vecIt);
        }
    }

    if (cm.nestedTypeCount >= contrib.nestedTypeCount)
    {
        cm.nestedTypeCount -= contrib.nestedTypeCount;
    }
    else
    {
        cm.nestedTypeCount = 0;
    }
    cm.hasNestedType = (cm.nestedTypeCount > 0);
}

/** @brief Removes container contributions of a partial index. */
void RemoveContainers(RuleIndex& index, const RuleIndexPartial& partial)
{
    for (const auto& [containerName, contrib] : partial.byContainer)
    {
        auto cIt = index.byContainer.find(containerName);
        if (cIt == index.byContainer.end())
        {
            continue;
        }

        RemoveContainerContribution(cIt->second, contrib);
        if (cIt->second.allMemberNames.empty() && cIt->second.nestedTypeCount == 0 && cIt->second.memberKeys.empty())
        {
            index.byContainer.erase(cIt);
        }
    }
}
} // namespace

RuleIndexPartial RuleIndex::BuildPartial(const std::string& fileUri, const std::vector<Symbol>& symbols)
{
    RuleIndexPartial partial;
    partial.fileUri = fileUri;
    PopulatePartial(partial, symbols);
    return partial;
}

RuleIndexPartial RuleIndex::BuildPartial(const std::string& fileUri, const std::vector<const Symbol*>& symbols)
{
    RuleIndexPartial partial;
    partial.fileUri = fileUri;
    PopulatePartial(partial, symbols);
    return partial;
}

void RuleIndex::ApplyPartial(const RuleIndexPartial& partial)
{
    ApplyNamesAndEnums(*this, partial);
    ApplyTypesAndAccessors(*this, partial);
    ApplyHierarchy(*this, partial);
    ApplyContainers(*this, partial);
}

void RuleIndex::RemovePartial(const RuleIndexPartial& partial)
{
    RemoveNamesAndEnums(*this, partial);
    RemoveTypesAndAccessors(*this, partial);
    RemoveHierarchy(*this, partial);
    RemoveContainers(*this, partial);
}

std::shared_ptr<RuleIndex> RuleIndex::Build(const SymbolTable& table)
{
    auto index = std::make_shared<RuleIndex>();

    table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            RuleIndexPartial partial = BuildPartial("", symbols);
            index->ApplyPartial(partial);
        });

    return index;
}
} // namespace angel_lsp::analysis::rules
