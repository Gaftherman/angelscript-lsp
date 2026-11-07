#include "analysis/index/HoverFacts.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "features/hover/HoverInternal.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
static bool IsTypeSymbol(const analysis::Symbol& sym) noexcept
{
    return sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface ||
           sym.type == analysis::SymbolType::Enum || sym.type == analysis::SymbolType::Funcdef ||
           sym.type == analysis::SymbolType::Typedef;
}

static void PromoteConstructorsToTypes(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols)
{
    std::vector<analysis::Symbol> promoted;
    for (const auto& s : symbols)
    {
        if (s.type == analysis::SymbolType::Function && !s.containerName.empty() &&
            analysis::LastScopeSegment(s.containerName) == s.name)
        {
            if (auto classSyms = ctx.request.symbolTable.FindSymbolsPtr(s.containerName))
            {
                for (const auto& cs : *classSyms)
                {
                    if (cs.type == analysis::SymbolType::Class || cs.type == analysis::SymbolType::Interface)
                    {
                        promoted.push_back(cs);
                    }
                }
            }
        }
    }
    symbols.insert(symbols.end(), promoted.begin(), promoted.end());
}

void DisambiguateTypeContextSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols)
{
    if (symbols.empty() || ctx.site.contextPreference != analysis::ContainerContextPreference::PreferClass)
    {
        return;
    }

    if (!std::any_of(symbols.begin(), symbols.end(), IsTypeSymbol))
    {
        PromoteConstructorsToTypes(ctx, symbols);
    }

    if (!std::any_of(symbols.begin(), symbols.end(), IsTypeSymbol))
    {
        return;
    }

    std::erase_if(symbols,
                  [](const analysis::Symbol& s)
                  {
                      return s.type == analysis::SymbolType::Function || s.type == analysis::SymbolType::Variable ||
                             s.type == analysis::SymbolType::Property || s.type == analysis::SymbolType::CallReference;
                  });

    auto it =
        std::find_if(symbols.begin(), symbols.end(), [](const analysis::Symbol& s)
                     { return s.type == analysis::SymbolType::Class || s.type == analysis::SymbolType::Interface; });
    if (it != symbols.end() && it != symbols.begin())
    {
        std::rotate(symbols.begin(), it, it + 1);
    }
}

void DisambiguateContainerSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols)
{
    if (symbols.size() < 2)
    {
        return;
    }
    auto classIt = std::find_if(symbols.begin(), symbols.end(),
                                [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Class; });
    auto nsIt = std::find_if(symbols.begin(), symbols.end(),
                             [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
    if (classIt == symbols.end() || nsIt == symbols.end() || classIt->name != nsIt->name)
    {
        return;
    }

    auto pref = ctx.site.contextPreference;
    if (pref == analysis::ContainerContextPreference::PreferClass)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
        return;
    }
    if (pref == analysis::ContainerContextPreference::PreferNamespace)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Class; });
        return;
    }

    if (ctx.site.nextScopedMemberRange.Empty())
    {
        return;
    }

    const std::string_view memberName = ctx.request.sourceCode.substr(ctx.site.nextScopedMemberRange.startByte,
                                                                      ctx.site.nextScopedMemberRange.endByte -
                                                                          ctx.site.nextScopedMemberRange.startByte);
    const bool inClass = analysis::MemberBelongsToClass(*classIt, memberName, ctx.request.symbolTable);
    const bool inNs = analysis::MemberBelongsToNamespace(*nsIt, memberName, ctx.request.symbolTable);
    if (inClass && !inNs)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
    }
    else if (inNs && !inClass)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Class; });
    }
}

static void FilterAndAddConstructorCandidates(const std::vector<analysis::Symbol>* list,
                                              const analysis::Symbol& classSym, std::string_view qName,
                                              std::vector<analysis::Symbol>& ctorSymbols)
{
    if (!list)
    {
        return;
    }
    for (const auto& s : *list)
    {
        if (s.type == analysis::SymbolType::Function && s.name == classSym.name &&
            (s.containerName == qName || s.containerName == classSym.name ||
             analysis::LastScopeSegment(s.containerName) == classSym.name))
        {
            if (std::none_of(ctorSymbols.begin(), ctorSymbols.end(), [&](const analysis::Symbol& existing)
                             { return existing.name == s.name && analysis::HasSameParameterList(existing, s); }))
            {
                ctorSymbols.push_back(s);
            }
        }
    }
}

static void CollectClassConstructors(const analysis::Symbol& sym, const HoverQueryContext& ctx,
                                     std::vector<analysis::Symbol>& ctorSymbols)
{
    const std::string_view qName =
        sym.qualifiedName.empty() ? std::string_view(sym.name) : std::string_view(sym.qualifiedName);
    auto ctors = ctx.request.symbolTable.FindMemberSymbolPtr(qName, sym.name);
    if ((!ctors || ctors->empty()) && qName != sym.name)
    {
        ctors = ctx.request.symbolTable.FindMemberSymbolPtr(sym.name, sym.name);
    }
    if (ctors)
    {
        ctorSymbols.insert(ctorSymbols.end(), ctors->begin(), ctors->end());
    }

    FilterAndAddConstructorCandidates(ctx.request.symbolTable.FindSymbolsPtr(sym.name).get(), sym, qName, ctorSymbols);
    if (qName != sym.name)
    {
        FilterAndAddConstructorCandidates(ctx.request.symbolTable.FindSymbolsPtr(qName).get(), sym, qName, ctorSymbols);
    }
}

void AppendConstructorSymbolsIfCall(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols)
{
    if (ctx.site.enclosingCallRange.Empty())
    {
        return;
    }

    std::vector<analysis::Symbol> ctorSymbols;
    for (const auto& sym : symbols)
    {
        if (sym.type == analysis::SymbolType::Class)
        {
            CollectClassConstructors(sym, ctx, ctorSymbols);
        }
    }
    if (!ctorSymbols.empty())
    {
        analysis::ExpressionTypeContext exprCtx(ctx.scope, ctx.request.symbolTable, ctx.request.sourceCode,
                                                ctx.request.uri, ctx.request.nodeIndex);
        auto best =
            analysis::ResolveCallOverload(ctx.site.targetStartByte, ctx.site.targetEndByte, ctorSymbols, exprCtx);
        if (best.has_value())
        {
            auto it = std::find_if(ctorSymbols.begin(), ctorSymbols.end(), [&](const analysis::Symbol& s)
                                   { return s.name == best->name && analysis::HasSameParameterList(s, *best); });
            if (it != ctorSymbols.end() && it != ctorSymbols.begin())
            {
                std::rotate(ctorSymbols.begin(), it, it + 1);
            }
            symbols = std::move(ctorSymbols);
        }
        else
        {
            for (auto&& cs : ctorSymbols)
            {
                symbols.push_back(std::move(cs));
            }
        }
    }
}

void FilterDirectRootGlobalSymbols(std::vector<analysis::Symbol>& symbols, bool isDirectRootGlobal)
{
    if (isDirectRootGlobal)
    {
        std::erase_if(symbols, [](const analysis::Symbol& s)
                      { return !s.containerName.empty() || s.type == analysis::SymbolType::CallReference; });
    }
}
} // namespace angel_lsp::features
