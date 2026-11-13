#include "analysis/resolution/MixinInclusion.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ReferenceCountingScope.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>

namespace angel_lsp::analysis
{
MixinInclusionMap BuildMixinInclusionMap(const SymbolTable& symbolTable)
{
    MixinInclusionMap map;
    symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            for (const auto& cand : symbols)
            {
                if (cand.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cand.signature))
                {
                    const auto& cls = cand.GetClass();
                    for (const auto& m : cls.includedMixins)
                    {
                        auto& list = map[m];
                        list.push_back(cand.name);
                        if (!cand.qualifiedName.empty() && cand.qualifiedName != cand.name)
                        {
                            list.push_back(cand.qualifiedName);
                        }
                        auto candDerived = GetDerivedClasses(cand.name, symbolTable);
                        for (const auto& rel : candDerived)
                        {
                            list.push_back(rel);
                        }
                    }
                }
            }
        });
    return map;
}

ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>
CollectCompatibleClasses(std::span<const Symbol> symGroup, const Symbol& sym, AccessModifier targetAccess,
                         const MixinClassContext& ctx)
{
    ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>> compatibleClasses;
    for (const auto& s : symGroup)
    {
        if (IsSymbolInClassContainer(s, ctx.symbolTable))
        {
            compatibleClasses.insert(s.containerName);
            compatibleClasses.emplace(utils::LastScopeSegment(s.containerName));
            auto comp = GetCompatibleMemberClasses(s.containerName, sym.name, targetAccess, ctx.symbolTable);
            for (const auto& c : comp)
            {
                compatibleClasses.insert(c);
            }
        }
    }

    if (!compatibleClasses.empty())
    {
        std::vector<std::string> initial(compatibleClasses.begin(), compatibleClasses.end());
        for (const auto& c : initial)
        {
            auto it = ctx.mixinMap.find(c);
            if (it != ctx.mixinMap.end())
            {
                for (const auto& candClass : it->second)
                {
                    compatibleClasses.insert(candClass);
                }
            }
        }
    }
    return compatibleClasses;
}

std::optional<SourceRange> FindTypeReferenceRange(const Scope& root, std::string_view name, uint32_t firstLine,
                                                  uint32_t lastLine)
{
    const std::string_view shortName = utils::LastScopeSegment(name);
    std::optional<SourceRange> bestMatch;
    std::vector<const Scope*> stack{&root};
    while (!stack.empty())
    {
        const Scope* scope = stack.back();
        stack.pop_back();
        if (!scope)
        {
            continue;
        }

        for (const auto& ref : scope->references)
        {
            if (ref.isTypeSpecifier && (ref.name == name || ref.name == shortName) && ref.startLine >= firstLine &&
                ref.startLine <= lastLine)
            {
                SourceRange range{ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter};
                if (!bestMatch || range.startLine < bestMatch->startLine ||
                    (range.startLine == bestMatch->startLine && range.startCharacter < bestMatch->startCharacter))
                {
                    bestMatch = range;
                }
            }
        }

        for (const auto& child : scope->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
    return bestMatch;
}

} // namespace angel_lsp::analysis
