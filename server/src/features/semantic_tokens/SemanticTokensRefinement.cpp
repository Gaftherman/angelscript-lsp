#include "analysis/resolution/ClassHierarchyResolver.h"
#include "features/semantic_tokens/SemanticTokensInternal.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

[[nodiscard]] static inline uint32_t MatchAgreedTypeToken(analysis::SymbolType agreedType, bool isEnumConstant) noexcept
{
    if (agreedType == analysis::SymbolType::Class)
        return analysis::Type_Class;
    if (agreedType == analysis::SymbolType::Interface)
        return analysis::Type_Interface;
    if (agreedType == analysis::SymbolType::Enum)
        return analysis::Type_Enum;
    if (agreedType == analysis::SymbolType::Variable && isEnumConstant)
        return analysis::Type_EnumMember;
    return analysis::Type_Type;
}

[[nodiscard]] static inline uint32_t MatchAgreedNamespaceToken(analysis::SymbolType agreedType) noexcept
{
    if (agreedType == analysis::SymbolType::Enum)
        return analysis::Type_Enum;
    if (agreedType == analysis::SymbolType::Class)
        return analysis::Type_Class;
    return analysis::Type_Namespace;
}

[[nodiscard]] static inline uint32_t MatchAgreedVariableToken(bool isEnumConstant, bool hasContainer) noexcept
{
    if (isEnumConstant)
    {
        return analysis::Type_EnumMember;
    }
    if (hasContainer)
    {
        return analysis::Type_Property;
    }
    return analysis::Type_Variable;
}

static uint32_t MatchAgreedSymbolType(analysis::SymbolType agreedType, uint32_t tokenType, bool hasContainer,
                                      bool isEnumConstant)
{
    if (tokenType == analysis::Type_Type)
    {
        return MatchAgreedTypeToken(agreedType, isEnumConstant);
    }
    if (tokenType == analysis::Type_Namespace)
    {
        return MatchAgreedNamespaceToken(agreedType);
    }
    if (agreedType == analysis::SymbolType::Function && tokenType == analysis::Type_Function && hasContainer)
    {
        return analysis::Type_Method;
    }
    if (agreedType == analysis::SymbolType::Variable &&
        (tokenType == analysis::Type_Variable || tokenType == analysis::Type_Function ||
         tokenType == analysis::Type_Method))
    {
        return MatchAgreedVariableToken(isEnumConstant, hasContainer);
    }
    return tokenType;
}

static uint32_t RefineRuleIndexFallback(std::string_view tokenText, uint32_t tokenType,
                                        const TokenRefinementContext& ctx)
{
    if (!ctx.ruleIndex)
    {
        return tokenType;
    }
    if ((tokenType == analysis::Type_Variable || tokenType == analysis::Type_Type) &&
        ctx.ruleIndex->enumMemberNames.contains(tokenText))
    {
        return analysis::Type_EnumMember;
    }
    if (tokenType == analysis::Type_Variable && (ctx.ruleIndex->globalAccessorPropertyNames.contains(tokenText) ||
                                                 ctx.ruleIndex->keywordGlobalAccessorPropertyNames.contains(tokenText)))
    {
        return analysis::Type_Property;
    }
    if (tokenType == analysis::Type_Type)
    {
        auto it = ctx.ruleIndex->qualifiedTypesByShortName.find(tokenText);
        if (it != ctx.ruleIndex->qualifiedTypesByShortName.end() && !it->second.empty())
        {
            const auto syms = ctx.request.symbolTable.FindSymbolsPtr(it->second.front());
            if (syms && !syms->empty())
            {
                return MatchAgreedSymbolType(syms->front().type, tokenType, true, false);
            }
        }
    }
    return tokenType;
}

static uint32_t RefineSymbolTableMatch(std::string_view tokenText, uint32_t tokenType,
                                       const TokenRefinementContext& ctx)
{
    const auto symbols = ctx.request.symbolTable.FindSymbolsPtr(tokenText);
    if (!symbols || symbols->empty())
    {
        return RefineRuleIndexFallback(tokenText, tokenType, ctx);
    }
    if (tokenType == analysis::Type_Type)
    {
        auto typeIt =
            std::find_if(symbols->begin(), symbols->end(),
                         [](const analysis::Symbol& s)
                         {
                             return s.type == analysis::SymbolType::Class ||
                                    s.type == analysis::SymbolType::Interface || s.type == analysis::SymbolType::Enum ||
                                    s.type == analysis::SymbolType::Typedef || s.type == analysis::SymbolType::Funcdef;
                         });
        if (typeIt != symbols->end())
        {
            return MatchAgreedSymbolType(typeIt->type, tokenType, !typeIt->containerName.empty(), false);
        }
    }
    else if (tokenType == analysis::Type_Namespace)
    {
        auto nsIt = std::find_if(symbols->begin(), symbols->end(),
                                 [](const analysis::Symbol& s) { return s.type == analysis::SymbolType::Namespace; });
        if (nsIt != symbols->end())
        {
            return analysis::Type_Namespace;
        }
    }
    const analysis::SymbolType agreedType = symbols->front().type;
    bool allSymbolsMatch = std::all_of(symbols->begin(), symbols->end(),
                                       [agreedType](const analysis::Symbol& s) { return s.type == agreedType; });
    if (!allSymbolsMatch)
    {
        return tokenType;
    }
    bool isEnumConstant = false;
    if (agreedType == analysis::SymbolType::Variable &&
        std::holds_alternative<analysis::VariableSignature>(symbols->front().signature))
    {
        isEnumConstant = symbols->front().GetVariable().isEnumConstant;
    }
    return MatchAgreedSymbolType(agreedType, tokenType, !symbols->front().containerName.empty(), isEnumConstant);
}

uint32_t RefineEnclosingClassMember(std::string_view tokenText, uint32_t startByte, uint32_t tokenType,
                                    const TokenRefinementContext& ctx)
{
    std::string_view className = ctx.request.facts.FindEnclosingClass(startByte);
    if (className.empty() || !ctx.ruleIndex)
    {
        return tokenType;
    }

    uint32_t refinedType = tokenType;
    ctx.classHierarchyCache.AnyInheritedType(
        className, ctx.request.symbolTable,
        [&](std::string_view owner)
        {
            const auto& ownerMembers = ctx.ruleIndex->ClassMembers(owner);
            if (tokenType == analysis::Type_Function && ownerMembers.methodNames.contains(tokenText))
            {
                refinedType = analysis::Type_Method;
                return true;
            }
            if (tokenType == analysis::Type_Variable && ownerMembers.allMemberNames.contains(tokenText))
            {
                refinedType = analysis::Type_Property;
                return true;
            }
            return false;
        });
    return refinedType;
}

uint32_t RefineSyntaxContext(std::string_view tokenText, uint32_t startByte, uint32_t tokenType,
                             const TokenRefinementContext& ctx)
{
    if (auto it = ctx.scopedEnumUpgrades.find(startByte); it != ctx.scopedEnumUpgrades.end())
    {
        tokenType = it->second;
    }
    if (tokenType == analysis::Type_Variable || tokenType == analysis::Type_Function)
    {
        tokenType = RefineEnclosingClassMember(tokenText, startByte, tokenType, ctx);
    }
    if (tokenType == analysis::Type_Type || tokenType == analysis::Type_Variable ||
        tokenType == analysis::Type_Function || tokenType == analysis::Type_Namespace)
    {
        tokenType = RefineSymbolTableMatch(tokenText, tokenType, ctx);
    }
    return tokenType;
}

uint32_t RefineCapturedTokenType(const analysis::IndexedHighlightCapture& capture, uint32_t tokenType,
                                 const TokenRefinementContext& ctx)
{
    const uint32_t startByte = capture.startByte;
    const uint32_t endByte = capture.endByte;
    if (startByte < endByte && endByte <= ctx.request.sourceCode.size())
    {
        const std::string_view tokenText(ctx.request.sourceCode.data() + startByte, endByte - startByte);
        if (auto kwType = ClassifyReservedKeywordOrModifier(tokenText))
        {
            return *kwType;
        }
    }

    if (tokenType == analysis::Type_Variable)
    {
        tokenType = ResolveReferenceTokenType(capture.startLine, capture.startColumn, ctx.referenceKinds, tokenType);
    }

    if (tokenType == analysis::Type_Type || tokenType == analysis::Type_Variable ||
        tokenType == analysis::Type_Function || tokenType == analysis::Type_Namespace)
    {
        if (startByte < endByte && endByte <= ctx.request.sourceCode.size())
        {
            std::string_view tokenText(ctx.request.sourceCode.data() + startByte, endByte - startByte);
            if (!tokenText.empty())
            {
                if (ShouldSuppressScopedLeafToken(capture, tokenText, ctx))
                {
                    return UINT32_MAX;
                }
                tokenType = RefineSyntaxContext(tokenText, startByte, tokenType, ctx);
            }
        }
    }

    return tokenType;
}

} // namespace angel_lsp::features
