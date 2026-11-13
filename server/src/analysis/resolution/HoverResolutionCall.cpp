#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/HoverFacts.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ConversionRankingEngine.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "config/ServerConfig.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

std::string ResolveReceiverTypeName(IndexedByteRange objectRange, const ExpressionTypeContext& ctx,
                                    const config::ServerConfig* config)
{
    const config::EngineRuleConfig* engineRules = config ? &config->engineRules : nullptr;
    ReceiverTypeContext recvCtx{ctx.scope, ctx.uri, ctx.symbolTable.GetDefaultArrayType(), nullptr, engineRules};
    recvCtx.nodeIndex = ctx.nodeIndex;
    std::string receiverTypeName = analysis::ResolveReceiverType(objectRange, ctx.sourceCode, ctx.symbolTable, recvCtx);
    if (!receiverTypeName.empty() && config && !config->types.arrayTypeName.empty())
    {
        receiverTypeName = analysis::MemberOwnerType(receiverTypeName, config->types.arrayTypeName);
    }

    if (!receiverTypeName.empty())
    {
        receiverTypeName = ctx.symbolTable.QualifyShortTypeName(receiverTypeName);
    }
    return receiverTypeName;
}

std::vector<Symbol> CollectReceiverMemberSymbols(std::string_view receiverTypeName, std::string_view memberName,
                                                 const SymbolTable& symbolTable)
{
    std::vector<Symbol> memberSymbols;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, symbolTable);
    for (const auto& typeName : hierarchy)
    {
        if (const auto found = symbolTable.FindMemberSymbolPtr(typeName, memberName))
        {
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Function)
                {
                    bool overriddenLower =
                        std::any_of(memberSymbols.begin(), memberSymbols.end(),
                                    [&](const Symbol& kept) { return analysis::HasSameParameterList(kept, sym); });
                    if (!overriddenLower)
                    {
                        memberSymbols.push_back(sym);
                    }
                }
                else
                {
                    memberSymbols.push_back(sym);
                }
            }
        }
    }
    std::stable_partition(memberSymbols.begin(), memberSymbols.end(),
                          [](const Symbol& s)
                          {
                              if (s.type == SymbolType::Variable &&
                                  std::holds_alternative<VariableSignature>(s.signature))
                              {
                                  return !s.GetVariable().isEnumConstant;
                              }
                              return true;
                          });
    return memberSymbols;
}

std::vector<Symbol> ResolveNamedArgCalleeCandidates(IndexedByteRange callRange, const ExpressionTypeContext& ctx,
                                                    const config::ServerConfig* config)
{
    if (!ctx.nodeIndex)
    {
        return {};
    }
    const auto callFacts = BuildCallSiteFacts(*ctx.nodeIndex, ctx.sourceCode);
    const auto* site = callFacts.FindCallSite(callRange);
    if (!site)
    {
        return {};
    }

    if (site->isMemberCall)
    {
        std::string receiverTypeName = ResolveReceiverTypeName(site->objectOperand.bytes, ctx, config);
        return CollectReceiverMemberSymbols(receiverTypeName, site->calleeName, ctx.symbolTable);
    }

    if (const auto found = ctx.symbolTable.FindSymbolsPtr(site->calleeText))
    {
        return *found;
    }
    return {};
}

std::optional<Symbol> ResolveConstructorForDeclarator(uint32_t declStartByte, uint32_t declEndByte,
                                                      std::string_view typeName, const ExpressionTypeContext& ctx)
{
    std::string_view baseName = analysis::CleanBaseTypeView(typeName);
    if (baseName.empty() || parser::primitives::IsPrimitive(baseName) || !ctx.nodeIndex)
    {
        return std::nullopt;
    }

    auto candidates =
        analysis::CollectConstructorCandidates(baseName, declStartByte, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
    if (candidates.empty())
    {
        return std::nullopt;
    }
    if (candidates.size() == 1)
    {
        return candidates[0];
    }

    const auto callFacts = BuildCallSiteFacts(*ctx.nodeIndex, ctx.sourceCode);
    const DirectInitSiteFact* initSite = nullptr;
    uint32_t bestSpan = UINT32_MAX;
    for (const auto& init : callFacts.directInits)
    {
        if (init.bytes.startByte <= declStartByte && declEndByte <= init.bytes.endByte)
        {
            uint32_t span = init.bytes.endByte - init.bytes.startByte;
            if (span < bestSpan)
            {
                bestSpan = span;
                initSite = &init;
            }
        }
    }
    if (!initSite)
    {
        return candidates[0];
    }

    std::vector<std::string> argTypes;
    argTypes.reserve(initSite->argumentCount);
    for (uint32_t i = 0; i < initSite->argumentCount; ++i)
    {
        const auto& arg = callFacts.arguments[initSite->argumentBegin + i];
        argTypes.push_back(ResolveExpressionTypeAt(arg.bytes, ctx));
    }

    auto match = analysis::ResolveBestOverload(candidates, argTypes, ctx.symbolTable);
    if (match.bestCandidate != nullptr)
    {
        return *match.bestCandidate;
    }
    if (match.priorityCandidate != nullptr)
    {
        return *match.priorityCandidate;
    }

    const Symbol* fallback = analysis::FindBestFallbackOverload(candidates, argTypes, ctx.symbolTable);
    if (fallback != nullptr)
    {
        return *fallback;
    }

    return candidates[0];
}

std::string ResolveConstructorSignature(uint32_t nodeStartByte, uint32_t nodeEndByte, std::string_view typeName,
                                        const ExpressionTypeContext& ctx)
{
    if (auto ctorSym = ResolveConstructorForDeclarator(nodeStartByte, nodeEndByte, typeName, ctx))
    {
        return analysis::FormatFunctionDeclaration(*ctorSym);
    }
    return "";
}

std::optional<Symbol> ResolveCallOverload(uint32_t startByte, uint32_t endByte, std::span<const Symbol> candidates,
                                          const ExpressionTypeContext& ctx)
{
    if (candidates.empty())
    {
        return std::nullopt;
    }
    if (candidates.size() == 1)
    {
        return candidates.front();
    }
    if (!ctx.nodeIndex)
    {
        return candidates.front();
    }

    const auto callFacts = BuildCallSiteFacts(*ctx.nodeIndex, ctx.sourceCode);
    const CallSiteFact* bestSite = nullptr;
    uint32_t bestSpan = UINT32_MAX;
    for (const auto& site : callFacts.callSites)
    {
        if (site.bytes.startByte <= startByte && endByte <= site.bytes.endByte)
        {
            uint32_t span = site.bytes.endByte - site.bytes.startByte;
            if (span < bestSpan)
            {
                bestSpan = span;
                bestSite = &site;
            }
        }
    }
    if (!bestSite)
    {
        return candidates.front();
    }

    std::vector<std::string> argTypes;
    argTypes.reserve(bestSite->argumentCount);
    for (uint32_t i = 0; i < bestSite->argumentCount; ++i)
    {
        const auto& arg = callFacts.arguments[bestSite->argumentBegin + i];
        argTypes.push_back(ResolveExpressionTypeAt(arg.bytes, ctx));
    }

    auto match = analysis::ResolveBestOverload(candidates, argTypes, ctx.symbolTable);
    if (match.bestCandidate != nullptr)
    {
        return *match.bestCandidate;
    }
    if (match.priorityCandidate != nullptr)
    {
        return *match.priorityCandidate;
    }

    const Symbol* fallback = analysis::FindBestFallbackOverload(candidates, argTypes, ctx.symbolTable);
    if (fallback != nullptr)
    {
        return *fallback;
    }
    return candidates.front();
}

} // namespace angel_lsp::analysis
