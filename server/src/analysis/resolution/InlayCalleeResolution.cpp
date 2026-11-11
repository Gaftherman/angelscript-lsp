#include "analysis/resolution/InlayCalleeResolution.h"

#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/InlayCalleeResolutionInternal.h"
#include "analysis/resolution/InlayResolutionInternal.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/ScopedNameUtils.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Collects constructor candidates for class symbols using fully qualified names with fallback.
 * @param[in] candidateSymbols Symbols found in scope or table.
 * @param[in] symbolTable Symbol table for resolution.
 * @return Vector of constructor function symbols.
 */
static std::vector<Symbol> CollectClassConstructorCandidates(const std::vector<Symbol>& candidateSymbols,
                                                             const SymbolTable& symbolTable)
{
    std::vector<Symbol> ctorSymbols;
    for (const auto& sym : candidateSymbols)
    {
        if (sym.type == SymbolType::Class)
        {
            const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            auto ctors = symbolTable.FindMemberSymbols(qName, sym.name);
            if (ctors.empty() && qName != sym.name)
            {
                ctors = symbolTable.FindMemberSymbols(sym.name, sym.name);
            }
            ctorSymbols.insert(ctorSymbols.end(), ctors.begin(), ctors.end());
        }
    }
    return ctorSymbols;
}

/**
 * @brief Collects candidate function symbols for a free or unqualified call expression.
 * @param[in] calleeRawName Raw callee identifier text.
 * @param[in] callByteOffset Byte offset of the call site.
 * @param[in] ctx Semantic expression type context.
 * @return Vector of candidate symbols.
 */
static std::vector<Symbol> CollectFreeCalleeCandidates(std::string_view calleeRawName, uint32_t callByteOffset,
                                                       const ExpressionTypeContext& ctx)
{
    std::string calleeName = std::string(utils::StripGlobalPrefix(calleeRawName));
    auto candidateSymbols =
        FindSymbolsInScope(calleeName, callByteOffset, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);

    CollectEnclosingClassCallees(calleeName, callByteOffset, ctx, candidateSymbols);

    if (candidateSymbols.empty())
    {
        candidateSymbols = ctx.symbolTable.FindSymbols(calleeName);
    }

    if (candidateSymbols.empty())
    {
        candidateSymbols = CollectScopedCalleeCandidates(calleeName, callByteOffset, ctx);
    }

    auto ctorSymbols = CollectClassConstructorCandidates(candidateSymbols, ctx.symbolTable);
    if (!ctorSymbols.empty())
    {
        return ctorSymbols;
    }

    if (candidateSymbols.empty())
    {
        auto fallbackCtors =
            CollectConstructorCandidates(calleeName, callByteOffset, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        if (!fallbackCtors.empty())
        {
            return fallbackCtors;
        }
    }

    if (candidateSymbols.empty() && ctx.scope)
    {
        if (auto callableSym = TryResolveCallableFuncdef(calleeName, ctx.scope, ctx.symbolTable))
        {
            candidateSymbols.push_back(std::move(*callableSym));
        }
    }

    return candidateSymbols;
}

std::optional<ResolvedCallee> ResolveCallSiteCallee(const IndexedCallSite& site,
                                                    std::span<const IndexedCallArgument> args,
                                                    const ExpressionTypeContext& ctx)
{
    if (site.isConstructorCall)
    {
        std::string_view typeText = site.constructTypeName;
        if (typeText.empty())
        {
            return std::nullopt;
        }
        ResolvedCallee callee = ResolveConstructorParameters(typeText, site.startByte, ctx, args);
        if (callee.calleeName.empty() && callee.parameters.empty())
        {
            return std::nullopt;
        }
        return callee;
    }

    std::vector<Symbol> candidateSymbols = site.isMemberCall
                                               ? CollectMemberCalleeCandidates(site, ctx)
                                               : CollectFreeCalleeCandidates(site.calleeText, site.startByte, ctx);

    const Symbol* bestSym = nullptr;
    if (candidateSymbols.size() > 1)
    {
        std::vector<std::string> argTypes;
        argTypes.reserve(args.size());
        for (const auto& arg : args)
        {
            argTypes.push_back(ResolveExpressionTypeAt(IndexedByteRange{arg.startByte, arg.endByte}, ctx));
        }
        auto match = ResolveBestOverload(candidateSymbols, argTypes, ctx.symbolTable);
        if (match.bestCandidate != nullptr)
        {
            bestSym = match.bestCandidate;
        }
    }

    if (!bestSym)
    {
        bestSym = SelectCandidateByArgCount(candidateSymbols, args.size());
    }
    if (!bestSym && !candidateSymbols.empty())
    {
        bestSym = SelectFallbackCandidate(candidateSymbols);
    }

    ResolvedCallee callee = ExtractCalleeFromSymbol(bestSym);
    if (callee.calleeName.empty() && callee.parameters.empty())
    {
        return std::nullopt;
    }
    return callee;
}

} // namespace angel_lsp::analysis
