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

                    // A template class's parameters are names it introduces, so `T` inside
                    // `class array<T>` is declared even though nothing declares it separately.
                    //
                    // Registered globally rather than scoped to the class, deliberately. Correct
                    // scoping would mean threading a per-container name set through the identifier
                    // check, and it would buy nothing: template classes only appear in predefined
                    // stubs, where a missed shadowing costs nothing while the alternative cost eight
                    // false "Undeclared identifier 'T'" on every open of the engine's own API file.
                    // Guarded on the variant, not just the kind: this walks every symbol in the
                    // workspace, and error recovery can leave one carrying the kind without the
                    // signature to match. GetClass() on that throws.
                    if (sym.type == SymbolType::Class && std::holds_alternative<ClassSignature>(sym.signature))
                    {
                        for (const auto &param : sym.GetClass().templateParams)
                        {
                            index->allNames.insert(param);
                        }
                    }

                    if (sym.type == SymbolType::Enum && std::holds_alternative<EnumSignature>(sym.signature))
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
                        if (std::holds_alternative<FunctionSignature>(sym.signature) &&
                            sym.GetFunction().modifiers.isFinal)
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
