#include "analysis/rules/RuleIndex.h"
#include "analysis/SemanticHelpers.h"
#include <algorithm>

namespace angel_lsp::analysis::rules
{
    const ContainerMembers &RuleIndex::Members(std::string_view containerName) const
    {
        static const ContainerMembers empty;
        const auto it = byContainer.find(containerName);
        return it == byContainer.end() ? empty : it->second;
    }

    const ContainerMembers &RuleIndex::Members(const std::string &containerName) const
    {
        return Members(std::string_view(containerName));
    }

    void RuleIndexPartial::Merge(RuleIndexPartial &&other)
    {
        allNames.insert(allNames.end(),
                        std::make_move_iterator(other.allNames.begin()),
                        std::make_move_iterator(other.allNames.end()));

        enumMembers.insert(enumMembers.end(),
                           std::make_move_iterator(other.enumMembers.begin()),
                           std::make_move_iterator(other.enumMembers.end()));

        qualifiedTypes.insert(qualifiedTypes.end(),
                              std::make_move_iterator(other.qualifiedTypes.begin()),
                              std::make_move_iterator(other.qualifiedTypes.end()));

        accessorProperties.insert(accessorProperties.end(),
                                  std::make_move_iterator(other.accessorProperties.begin()),
                                  std::make_move_iterator(other.accessorProperties.end()));

        keywordAccessorProperties.insert(keywordAccessorProperties.end(),
                                         std::make_move_iterator(other.keywordAccessorProperties.begin()),
                                         std::make_move_iterator(other.keywordAccessorProperties.end()));

        derivedByBase.insert(derivedByBase.end(),
                             std::make_move_iterator(other.derivedByBase.begin()),
                             std::make_move_iterator(other.derivedByBase.end()));

        hostClassesByMixin.insert(hostClassesByMixin.end(),
                                  std::make_move_iterator(other.hostClassesByMixin.begin()),
                                  std::make_move_iterator(other.hostClassesByMixin.end()));

        for (auto &[containerName, contrib] : other.byContainer)
        {
            auto &target = byContainer[containerName];
            target.methodNames.insert(target.methodNames.end(),
                                      std::make_move_iterator(contrib.methodNames.begin()),
                                      std::make_move_iterator(contrib.methodNames.end()));
            target.finalMethodNames.insert(target.finalMethodNames.end(),
                                           std::make_move_iterator(contrib.finalMethodNames.begin()),
                                           std::make_move_iterator(contrib.finalMethodNames.end()));
            target.allMemberNames.insert(target.allMemberNames.end(),
                                         std::make_move_iterator(contrib.allMemberNames.begin()),
                                         std::make_move_iterator(contrib.allMemberNames.end()));
            target.memberKeys.insert(target.memberKeys.end(),
                                     std::make_move_iterator(contrib.memberKeys.begin()),
                                     std::make_move_iterator(contrib.memberKeys.end()));
            target.nestedTypeCount += contrib.nestedTypeCount;
        }
    }

    RuleIndexPartial RuleIndex::BuildPartial(const std::string &fileUri, const std::vector<Symbol> &symbols)
    {
        RuleIndexPartial partial;
        partial.fileUri = fileUri;

        for (const auto &sym : symbols)
        {
            if (sym.isSynthesized)
            {
                continue;
            }

            partial.allNames.push_back(sym.name);

            if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
            {
                for (const auto &param : sym.GetClass().templateParams)
                {
                    partial.allNames.push_back(param);
                }
            }

            if (sym.type == SymbolType::Namespace)
            {
                std::string_view remaining = sym.name;
                for (size_t at = remaining.find("::"); at != std::string_view::npos;
                     at = remaining.find("::"))
                {
                    partial.allNames.push_back(std::string(remaining.substr(0, at)));
                    remaining.remove_prefix(at + 2);
                }
                if (!remaining.empty())
                {
                    partial.allNames.push_back(std::string(remaining));
                }
            }

            if (sym.type == SymbolType::Enum && std::holds_alternative<EnumSignature>(sym.signature))
            {
                for (const auto &member : sym.GetEnum().members)
                {
                    partial.enumMembers.emplace_back(member.name, sym);
                }
            }

            if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface ||
                sym.type == SymbolType::Enum || sym.type == SymbolType::Typedef || sym.type == SymbolType::Funcdef)
            {
                const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                partial.qualifiedTypes.emplace_back(sym.name, qName);
            }

            if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
            {
                const DerivedType derived{
                    sym.qualifiedName.empty() ? sym.name : sym.qualifiedName,
                    sym.name
                };

                const auto recordBase = [&](const std::string &base)
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
                };

                if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
                {
                    for (const auto &base : sym.GetClass().bases)
                    {
                        recordBase(base);
                    }

                    for (const auto &mixinName : sym.GetClass().includedMixins)
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
                    for (const auto &base : sym.GetInterface().inheritedInterfaces)
                    {
                        recordBase(base);
                    }
                }
            }

            if (sym.containerName.empty() && sym.type == SymbolType::Function)
            {
                std::string_view accessor = sym.name;
                if (accessor.starts_with("get_") || accessor.starts_with("set_"))
                {
                    accessor.remove_prefix(4);
                    if (!accessor.empty())
                    {
                        partial.accessorProperties.emplace_back(accessor);
                        if (std::holds_alternative<FunctionSignature>(sym.signature) &&
                            sym.GetFunction().modifiers.isProperty)
                        {
                            partial.keywordAccessorProperties.emplace_back(accessor);
                        }
                    }
                }
            }

            if (sym.containerName.empty())
            {
                continue;
            }

            auto &members = partial.byContainer[sym.containerName];
            members.allMemberNames.push_back(sym.name);
            members.memberKeys.push_back(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName);

            switch (sym.type)
            {
            case SymbolType::Function:
                members.methodNames.push_back(sym.name);
                if (std::holds_alternative<FunctionSignature>(sym.signature) &&
                    sym.GetFunction().modifiers.isFinal)
                {
                    members.finalMethodNames.push_back(sym.name);
                }

                {
                    std::string_view accessor = sym.name;
                    if (accessor.starts_with("get_") || accessor.starts_with("set_"))
                    {
                        accessor.remove_prefix(4);
                        if (!accessor.empty())
                        {
                            partial.accessorProperties.emplace_back(accessor);
                            if (std::holds_alternative<FunctionSignature>(sym.signature) &&
                                sym.GetFunction().modifiers.isProperty)
                            {
                                partial.keywordAccessorProperties.emplace_back(accessor);
                            }
                        }
                    }
                }
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

        return partial;
    }

    void RuleIndex::ApplyPartial(const RuleIndexPartial &partial)
    {
        for (const auto &name : partial.allNames)
        {
            ++allNames[name];
        }

        for (const auto &[name, sym] : partial.enumMembers)
        {
            if (++enumMemberCounts[name] == 1)
            {
                enumMemberNames.insert(name);
            }
            enumSymbolsByMemberName[name].push_back(sym);
        }

        for (const auto &[shortName, qName] : partial.qualifiedTypes)
        {
            auto &counts = qualifiedTypeCounts[shortName];
            if (++counts[qName] == 1)
            {
                qualifiedTypesByShortName[shortName].push_back(qName);
            }
        }

        for (const auto &p : partial.accessorProperties)
        {
            if (++accessorPropertyCounts[p] == 1)
            {
                accessorPropertyNames.insert(p);
            }
        }

        for (const auto &p : partial.keywordAccessorProperties)
        {
            if (++keywordAccessorPropertyCounts[p] == 1)
            {
                keywordAccessorPropertyNames.insert(p);
            }
        }

        for (const auto &[base, derived] : partial.derivedByBase)
        {
            derivedByBase[base].push_back(derived);
        }

        for (const auto &[mixin, host] : partial.hostClassesByMixin)
        {
            hostClassesByMixin[mixin].push_back(host);
        }

        for (const auto &[containerName, contrib] : partial.byContainer)
        {
            auto &cm = byContainer[containerName];
            for (const auto &m : contrib.methodNames)
            {
                if (++cm.methodCounts[m] == 1)
                {
                    cm.methodNames.insert(m);
                }
            }
            for (const auto &m : contrib.finalMethodNames)
            {
                if (++cm.finalMethodCounts[m] == 1)
                {
                    cm.finalMethodNames.insert(m);
                }
            }
            for (const auto &m : contrib.allMemberNames)
            {
                if (++cm.allMemberCounts[m] == 1)
                {
                    cm.allMemberNames.insert(m);
                }
            }
            for (const auto &k : contrib.memberKeys)
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
    }

    void RuleIndex::RemovePartial(const RuleIndexPartial &partial)
    {
        for (const auto &name : partial.allNames)
        {
            auto it = allNames.find(name);
            if (it != allNames.end())
            {
                if (--it->second == 0)
                {
                    allNames.erase(it);
                }
            }
        }

        for (const auto &[name, sym] : partial.enumMembers)
        {
            auto it = enumMemberCounts.find(name);
            if (it != enumMemberCounts.end())
            {
                if (--it->second == 0)
                {
                    enumMemberCounts.erase(it);
                    enumMemberNames.erase(name);
                }
            }
            auto sIt = enumSymbolsByMemberName.find(name);
            if (sIt != enumSymbolsByMemberName.end())
            {
                std::erase_if(sIt->second, [&](const Symbol &s)
                {
                    return s.fileUri == partial.fileUri && s.name == sym.name;
                });
                if (sIt->second.empty())
                {
                    enumSymbolsByMemberName.erase(sIt);
                }
            }
        }

        for (const auto &[shortName, qName] : partial.qualifiedTypes)
        {
            auto qIt = qualifiedTypeCounts.find(shortName);
            if (qIt != qualifiedTypeCounts.end())
            {
                auto cIt = qIt->second.find(qName);
                if (cIt != qIt->second.end())
                {
                    if (--cIt->second == 0)
                    {
                        qIt->second.erase(cIt);
                        auto vecIt = qualifiedTypesByShortName.find(shortName);
                        if (vecIt != qualifiedTypesByShortName.end())
                        {
                            std::erase(vecIt->second, qName);
                            if (vecIt->second.empty())
                            {
                                qualifiedTypesByShortName.erase(vecIt);
                            }
                        }
                    }
                }
                if (qIt->second.empty())
                {
                    qualifiedTypeCounts.erase(qIt);
                }
            }
        }

        for (const auto &p : partial.accessorProperties)
        {
            auto it = accessorPropertyCounts.find(p);
            if (it != accessorPropertyCounts.end())
            {
                if (--it->second == 0)
                {
                    accessorPropertyCounts.erase(it);
                    accessorPropertyNames.erase(p);
                }
            }
        }

        for (const auto &p : partial.keywordAccessorProperties)
        {
            auto it = keywordAccessorPropertyCounts.find(p);
            if (it != keywordAccessorPropertyCounts.end())
            {
                if (--it->second == 0)
                {
                    keywordAccessorPropertyCounts.erase(it);
                    keywordAccessorPropertyNames.erase(p);
                }
            }
        }

        for (const auto &[base, derived] : partial.derivedByBase)
        {
            auto it = derivedByBase.find(base);
            if (it != derivedByBase.end())
            {
                std::erase_if(it->second, [&](const DerivedType &d)
                {
                    return d.qualifiedName == derived.qualifiedName && d.name == derived.name;
                });
                if (it->second.empty())
                {
                    derivedByBase.erase(it);
                }
            }
        }

        for (const auto &[mixin, host] : partial.hostClassesByMixin)
        {
            auto it = hostClassesByMixin.find(mixin);
            if (it != hostClassesByMixin.end())
            {
                std::erase_if(it->second, [&](const DerivedType &d)
                {
                    return d.qualifiedName == host.qualifiedName && d.name == host.name;
                });
                if (it->second.empty())
                {
                    hostClassesByMixin.erase(it);
                }
            }
        }

        for (const auto &[containerName, contrib] : partial.byContainer)
        {
            auto cIt = byContainer.find(containerName);
            if (cIt == byContainer.end())
            {
                continue;
            }

            auto &cm = cIt->second;
            for (const auto &m : contrib.methodNames)
            {
                auto it = cm.methodCounts.find(m);
                if (it != cm.methodCounts.end() && --it->second == 0)
                {
                    cm.methodCounts.erase(it);
                    cm.methodNames.erase(m);
                }
            }
            for (const auto &m : contrib.finalMethodNames)
            {
                auto it = cm.finalMethodCounts.find(m);
                if (it != cm.finalMethodCounts.end() && --it->second == 0)
                {
                    cm.finalMethodCounts.erase(it);
                    cm.finalMethodNames.erase(m);
                }
            }
            for (const auto &m : contrib.allMemberNames)
            {
                auto it = cm.allMemberCounts.find(m);
                if (it != cm.allMemberCounts.end() && --it->second == 0)
                {
                    cm.allMemberCounts.erase(it);
                    cm.allMemberNames.erase(m);
                }
            }
            for (const auto &k : contrib.memberKeys)
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

            if (cm.allMemberNames.empty() && cm.nestedTypeCount == 0 && cm.memberKeys.empty())
            {
                byContainer.erase(cIt);
            }
        }
    }

    std::shared_ptr<RuleIndex> RuleIndex::Build(const SymbolTable &table)
    {
        auto index = std::make_shared<RuleIndex>();

        table.ForEachSymbol(
            [&](const std::string &, const std::vector<Symbol> &symbols)
            {
                RuleIndexPartial partial = BuildPartial("", symbols);
                index->ApplyPartial(partial);
            });

        return index;
    }
}
