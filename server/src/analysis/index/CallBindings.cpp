#include "analysis/index/CallBindings.h"

#include <functional>

namespace angel_lsp::analysis
{
CallDeclaration::CallDeclaration(const Symbol& symbol)
    : uri(symbol.fileUri), qualifiedName(symbol.qualifiedName), range(symbol.selectionRange),
      signature(symbol.GetDisplaySignature())
{
}

bool CallDeclaration::Matches(const Symbol& symbol) const noexcept
{
    return range == symbol.selectionRange && qualifiedName == symbol.qualifiedName && uri == symbol.fileUri &&
           signature == symbol.GetDisplaySignature();
}

size_t CallDeclarationHash::operator()(const CallDeclaration& declaration) const noexcept
{
    size_t hash = SourceRangeHash{}(declaration.range);
    const auto add = [&](std::string_view value) noexcept
    { hash ^= std::hash<std::string_view>{}(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2); };
    add(declaration.uri);
    add(declaration.qualifiedName);
    add(declaration.signature);
    return hash;
}

void CallBindings::Add(uint32_t nameByte, const SourceRange& use, const Symbol& declaration)
{
    auto [targetIt, inserted] = m_targets.try_emplace(nameByte, nullptr);
    if (!inserted)
    {
        return;
    }

    auto group = m_uses.try_emplace(CallDeclaration(declaration)).first;
    group->second.push_back(use);
    targetIt->second = &group->first;
}

const CallDeclaration* CallBindings::Find(uint32_t nameByte) const
{
    const auto target = m_targets.find(nameByte);
    return target == m_targets.end() ? nullptr : target->second;
}

} // namespace angel_lsp::analysis
