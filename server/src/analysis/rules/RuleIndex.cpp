#include "analysis/rules/RuleIndex.h"

namespace angel_lsp::analysis::rules
{
    const ContainerMembers &RuleIndex::Members(const std::string &containerName) const
    {
        static const ContainerMembers empty;
        const auto it = byContainer.find(containerName);
        return it == byContainer.end() ? empty : it->second;
    }

    std::shared_ptr<const RuleIndex> RuleIndex::Build(const SymbolTable &table)
    {
        auto index = std::make_shared<RuleIndex>();

        table.ForEachSymbol(
            [&](const std::string &, const std::vector<Symbol> &symbols)
            {
                for (const auto &sym : symbols)
                {
                    index->allNames.insert(sym.name);

                    if (sym.type == SymbolType::Enum)
                    {
                        for (const auto &member : sym.GetEnum().members)
                        {
                            index->enumMemberNames.insert(member.name);
                        }
                    }

                    if (sym.containerName.empty())
                    {
                        continue;
                    }

                    ContainerMembers &members = index->byContainer[sym.containerName];
                    switch (sym.type)
                    {
                    case SymbolType::Function:
                        members.methodNames.insert(sym.name);
                        if (sym.GetFunction().modifiers.isFinal)
                        {
                            members.finalMethodNames.insert(sym.name);
                        }
                        break;
                    case SymbolType::Class:
                    case SymbolType::Interface:
                    case SymbolType::Enum:
                    case SymbolType::Typedef:
                    case SymbolType::Funcdef:
                        members.hasNestedType = true;
                        break;
                    default:
                        break;
                    }
                }
            });

        return index;
    }
}
