#include "analysis/overload/ConversionRankingEngine.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/overload/OverloadTypeConversions.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace angel_lsp::analysis
{
namespace
{
std::optional<ArgumentConversion> EvaluateSpecialArgumentMatch(const std::string& argType,
                                                               const ParameterInformation& param,
                                                               const SymbolTable& symbolTable)
{
    if (IsWildcardParameter(param))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 220, 0, false, 10};
    }
    if (argType.empty() || argType == "auto")
    {
        return ArgumentConversion{ConversionRank::Exact, 0, 0, false, 0};
    }
    if (argType == "null")
    {
        const bool paramIsHandle = param.isHandle || HasHandleModifier(param.typeName);
        if (paramIsHandle)
        {
            return ArgumentConversion{ConversionRank::Exact, 0, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::Exact)};
        }
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    if (argType == "void")
    {
        if (IsOutParameter(param))
        {
            return ArgumentConversion{ConversionRank::Exact, 0, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::Exact)};
        }
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    if (argType == "init_list")
    {
        if (ParameterAcceptsInitializerList(param, &symbolTable, ""))
        {
            return ArgumentConversion{ConversionRank::Exact, 0, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::Exact)};
        }
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    return std::nullopt;
}

uint8_t IntegerBitWidth(std::string_view typeName)
{
    if (typeName == "int8" || typeName == "uint8")
    {
        return 8;
    }
    if (typeName == "int16" || typeName == "uint16")
    {
        return 16;
    }
    if (typeName == "int64" || typeName == "uint64")
    {
        return 64;
    }
    return 32;
}

bool IsMutableRefParam(const ParameterInformation& param)
{
    return param.isReference || param.modifier == ParameterModifier::InOut || param.modifier == ParameterModifier::Out;
}

std::optional<ArgumentConversion> EvaluateNumericMutableRef(const MatchContext& ctx)
{
    if (!IsNumericPrimitive(ctx.cleanArg) || !IsNumericPrimitive(ctx.cleanParam))
    {
        return std::nullopt;
    }
    if (IsIntegerType(ctx.cleanArg) && IsIntegerType(ctx.cleanParam))
    {
        if (IsUnsignedInteger(ctx.cleanArg) == IsUnsignedInteger(ctx.cleanParam))
        {
            if (IsPrimitiveWidening(ctx.cleanArg, ctx.cleanParam))
            {
                return ArgumentConversion{
                    ConversionRank::Promotion, 0, 0, false, static_cast<int>(OverloadMatchPenalty::Widening), false};
            }
            return ArgumentConversion{
                ConversionRank::StandardConv, 10, 0, false, static_cast<int>(OverloadMatchPenalty::Narrowing), true};
        }
        const uint8_t subRank = (IntegerBitWidth(ctx.cleanArg) == IntegerBitWidth(ctx.cleanParam)) ? 20 : 22;
        if (IsPrimitiveWidening(ctx.cleanArg, ctx.cleanParam))
        {
            return ArgumentConversion{ConversionRank::StandardConv,
                                      subRank,
                                      0,
                                      false,
                                      static_cast<int>(OverloadMatchPenalty::SignednessChange),
                                      false};
        }
        return ArgumentConversion{
            ConversionRank::StandardConv, 40, 0, false, static_cast<int>(OverloadMatchPenalty::SignednessChange), true};
    }
    if (IsFloatingPointType(ctx.cleanArg) && IsFloatingPointType(ctx.cleanParam))
    {
        if (IsPrimitiveWidening(ctx.cleanArg, ctx.cleanParam))
        {
            return ArgumentConversion{
                ConversionRank::Promotion, 0, 0, false, static_cast<int>(OverloadMatchPenalty::Widening), false};
        }
        return ArgumentConversion{
            ConversionRank::StandardConv, 10, 0, false, static_cast<int>(OverloadMatchPenalty::Narrowing), true};
    }
    if (IsIntegerType(ctx.cleanArg) && IsFloatingPointType(ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::StandardConv,
                                  30,
                                  0,
                                  false,
                                  static_cast<int>(OverloadMatchPenalty::WideningAcrossKind),
                                  false};
    }
    return ArgumentConversion{
        ConversionRank::StandardConv, 45, 0, false, static_cast<int>(OverloadMatchPenalty::Narrowing), true};
}

std::optional<ArgumentConversion> EvaluateMutableRefMatch(const ParameterInformation& param, const MatchContext& ctx)
{
    if (!ctx.isMutableRef)
    {
        return std::nullopt;
    }
    if (ctx.argIsConst)
    {
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    if (ctx.argIsHandle != ctx.paramIsHandle && !IsMutableRefParam(param))
    {
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    if (IsSameType(ctx.cleanArg, ctx.cleanParam))
    {
        const bool objectToHandleRef = !ctx.argIsHandle && ctx.paramIsHandle;
        return ArgumentConversion{ConversionRank::Exact, 0, 0, objectToHandleRef,
                                  objectToHandleRef ? static_cast<int>(OverloadMatchPenalty::ConstRef)
                                                    : static_cast<int>(OverloadMatchPenalty::Exact)};
    }
    if (auto numConv = EvaluateNumericMutableRef(ctx))
    {
        return numConv;
    }
    return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                              static_cast<int>(OverloadMatchPenalty::Incompatible)};
}

bool IsHandleToReferenceBinding(bool argIsHandle, bool paramIsHandle, const ParameterInformation& param)
{
    return argIsHandle && !paramIsHandle &&
           (param.isReference || param.modifier == ParameterModifier::In ||
            param.modifier == ParameterModifier::InOut || param.modifier == ParameterModifier::Out);
}

std::optional<ArgumentConversion> EvaluateSameTypeMatch(const MatchContext& ctx, bool isHandleToRef,
                                                        bool isValueToHandle)
{
    if (!IsSameType(ctx.cleanArg, ctx.cleanParam))
    {
        return std::nullopt;
    }
    if (ctx.argIsHandle == ctx.paramIsHandle)
    {
        const bool constDiff = (ctx.argIsConst != ctx.paramIsConst);
        return ArgumentConversion{ConversionRank::Exact, 0, 0, constDiff,
                                  constDiff ? static_cast<int>(OverloadMatchPenalty::ConstRef)
                                            : static_cast<int>(OverloadMatchPenalty::Exact)};
    }
    if (isHandleToRef || isValueToHandle)
    {
        if (ctx.paramIsConst || !ctx.argIsConst)
        {
            return ArgumentConversion{ConversionRank::Exact, 0, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::Exact)};
        }
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    return ArgumentConversion{ConversionRank::Exact, 0, 0, true, static_cast<int>(OverloadMatchPenalty::ConstRef)};
}

std::optional<ArgumentConversion> EvaluateInheritedSubtypeMatch(const MatchContext& ctx, bool isValueToHandle,
                                                                bool isHandleToRef)
{
    if (ctx.argIsHandle != ctx.paramIsHandle && !isValueToHandle && !isHandleToRef)
    {
        return std::nullopt;
    }

    const auto hierarchy = GetInheritedTypeHierarchy(ctx.cleanArg, ctx.table);
    for (size_t dist = 0; dist < hierarchy.size(); ++dist)
    {
        if (IsSameType(NormalizeType(hierarchy[dist]), ctx.cleanParam))
        {
            const uint8_t distance = static_cast<uint8_t>(std::min<size_t>(dist + 1, 255));
            const int basePenalty = static_cast<int>(OverloadMatchPenalty::Inheritance) + (isValueToHandle ? 1 : 0) +
                                    static_cast<int>(dist);
            return ArgumentConversion{ConversionRank::StandardConv, 0, distance, false, basePenalty};
        }
    }
    return std::nullopt;
}

std::optional<ArgumentConversion> EvaluateSameTypeOrSubtypeMatch(const ParameterInformation& param,
                                                                 const MatchContext& ctx)
{
    const bool isHandleToRef = IsHandleToReferenceBinding(ctx.argIsHandle, ctx.paramIsHandle, param);
    const bool isValueToHandle = !ctx.argIsHandle && ctx.paramIsHandle;

    if (auto sameConv = EvaluateSameTypeMatch(ctx, isHandleToRef, isValueToHandle))
    {
        return sameConv;
    }
    return EvaluateInheritedSubtypeMatch(ctx, isValueToHandle, isHandleToRef);
}

std::optional<ArgumentConversion> EvaluateEnumArgumentConversion(const MatchContext& ctx)
{
    const auto symbols = ctx.table.FindSymbolsPtr(ctx.cleanArg);
    const bool namesAnEnum = symbols && std::any_of(symbols->begin(), symbols->end(),
                                                    [](const Symbol& sym) { return sym.type == SymbolType::Enum; });
    if (!namesAnEnum)
    {
        return std::nullopt;
    }
    if (IsIntegerType(ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::Promotion, 0, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Widening)};
    }
    if (IsFloatingPointType(ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 30, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::WideningAcrossKind)};
    }
    return std::nullopt;
}

std::optional<ArgumentConversion> EvaluateNarrowingConversion(const MatchContext& ctx)
{
    if (!IsPrimitiveNarrowing(ctx.cleanArg, ctx.cleanParam))
    {
        return std::nullopt;
    }
    const bool crossesKind = (IsFloatingPointType(ctx.cleanArg) && IsIntegerType(ctx.cleanParam)) ||
                             (IsIntegerType(ctx.cleanArg) && IsFloatingPointType(ctx.cleanParam));
    if (crossesKind)
    {
        const uint8_t subRank = IsUnsignedInteger(ctx.cleanParam) ? 50 : 40;
        return ArgumentConversion{ConversionRank::StandardConv, subRank, 0, false, static_cast<int>(subRank), true};
    }
    return ArgumentConversion{
        ConversionRank::StandardConv, 10, 0, false, static_cast<int>(OverloadMatchPenalty::Narrowing), true};
}

std::optional<ArgumentConversion> EvaluatePrimitiveOrEnumConversion(const MatchContext& ctx)
{
    if (auto enumConv = EvaluateEnumArgumentConversion(ctx))
    {
        return enumConv;
    }
    if (IsIntegerType(ctx.cleanArg) && IsIntegerType(ctx.cleanParam) &&
        IsUnsignedInteger(ctx.cleanArg) != IsUnsignedInteger(ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 20, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::SignednessChange)};
    }
    if (IsPrimitiveWidening(ctx.cleanArg, ctx.cleanParam))
    {
        const bool crossesKind = IsIntegerType(ctx.cleanArg) && IsFloatingPointType(ctx.cleanParam);
        if (crossesKind)
        {
            return ArgumentConversion{ConversionRank::StandardConv, 30, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::WideningAcrossKind)};
        }
        return ArgumentConversion{ConversionRank::Promotion, 0, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Widening)};
    }
    return EvaluateNarrowingConversion(ctx);
}

ArgumentConversion EvaluateCustomOrUnresolvedConversion(const MatchContext& ctx)
{
    const auto userConv = CheckUserConversion(ctx.cleanArg, ctx.cleanParam, ctx.table);
    if (userConv.viable)
    {
        const uint8_t subRank = userConv.isExact ? 0 : 20;
        return ArgumentConversion{ConversionRank::UserDefined, subRank, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::UserDefined)};
    }

    if (AreIncompatibleTemplateTypes(ctx.cleanArg, ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }

    const auto isNamedAndUnresolved = [&ctx](const std::string& typeName)
    {
        if (typeName.empty() || IsCorePrimitive(typeName) || typeName == ctx.stringTypeName)
        {
            return false;
        }
        const std::string owner = MemberOwnerType(typeName, ctx.arrayTypeName);
        return !ctx.table.FindSymbolsPtr(typeName) && (owner.empty() || !ctx.table.FindSymbolsPtr(owner));
    };

    if (isNamedAndUnresolved(ctx.cleanArg) && isNamedAndUnresolved(ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 100, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::UnknownTypes)};
    }
    return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                              static_cast<int>(OverloadMatchPenalty::Incompatible)};
}

ArgumentConversion EvaluateConversionMatch(const MatchContext& ctx)
{
    if (ctx.cleanArg == "auto" || ctx.cleanParam == "auto")
    {
        return ArgumentConversion{ConversionRank::StandardConv, 100, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::UnknownTypes)};
    }
    if (auto conv = EvaluatePrimitiveOrEnumConversion(ctx))
    {
        return *conv;
    }
    return EvaluateCustomOrUnresolvedConversion(ctx);
}

bool IsMutableReferenceParam(const ParameterInformation& param, bool paramIsConst)
{
    const bool isRefOrOut =
        param.isReference || param.modifier == ParameterModifier::Out || param.modifier == ParameterModifier::InOut ||
        param.rawText.find("&inout") != std::string::npos || param.typeName.find("&inout") != std::string::npos ||
        param.rawText.find("&out") != std::string::npos || param.typeName.find("&out") != std::string::npos;
    return isRefOrOut && !paramIsConst && param.modifier != ParameterModifier::In &&
           param.rawText.find("&in") == std::string::npos;
}

ArgumentConversion EvaluateCandidateTypeMatch(const ParameterInformation& param, const MatchContext& ctx)
{
    if (auto refScore = EvaluateMutableRefMatch(param, ctx))
    {
        return *refScore;
    }
    if (auto exactScore = EvaluateSameTypeOrSubtypeMatch(param, ctx))
    {
        return *exactScore;
    }
    return EvaluateConversionMatch(ctx);
}
} // namespace

ArgumentConversion EvaluateArgumentConversion(const std::string& argType, const ParameterInformation& param,
                                              const SymbolTable& symbolTable, bool argIsLValue)
{
    if (auto specialScore = EvaluateSpecialArgumentMatch(argType, param, symbolTable))
    {
        if (!argIsLValue && IsOutParameter(param))
        {
            return ArgumentConversion{ConversionRank::StandardConv, 200, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::RValueToOutParam)};
        }
        return *specialScore;
    }

    const std::string cleanArg = UnwrapTypedef(NormalizeType(argType), symbolTable);
    const std::string cleanParam = UnwrapTypedef(NormalizeType(param.typeName), symbolTable);
    if (cleanArg.empty() || cleanParam.empty())
    {
        return ArgumentConversion{ConversionRank::StandardConv, 100, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::UnknownTypes)};
    }
    if (AreIncompatibleTemplateTypes(cleanArg, cleanParam))
    {
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }

    const bool paramIsConst = param.isConst || HasConstModifier(param.typeName);
    const bool isMutableRef = IsMutableReferenceParam(param, paramIsConst);

    const MatchContext ctx{cleanArg,
                           cleanParam,
                           HasHandleModifier(argType),
                           HasConstModifier(argType),
                           param.isHandle || HasHandleModifier(param.typeName),
                           paramIsConst,
                           isMutableRef,
                           symbolTable};

    ArgumentConversion conv = EvaluateCandidateTypeMatch(param, ctx);
    if (!argIsLValue && (isMutableRef || IsOutParameter(param)) && conv.IsViable())
    {
        return ArgumentConversion{ConversionRank::StandardConv, 200, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::RValueToOutParam)};
    }
    return conv;
}

int ScoreArgumentMatch(const std::string& argType, const ParameterInformation& param, const SymbolTable& symbolTable,
                       bool argIsLValue)
{
    return EvaluateArgumentConversion(argType, param, symbolTable, argIsLValue).legacyScore;
}

bool MatchesCallArity(const FunctionSignature& sig, uint32_t argCount) noexcept
{
    uint32_t requiredParams = 0;
    uint32_t maxParams = 0;
    bool isVariadic = false;
    for (const auto& param : sig.parameters)
    {
        if (param.rawText.find("...") != std::string::npos)
        {
            isVariadic = true;
            continue;
        }
        ++maxParams;
        if (param.defaultValue.empty())
        {
            ++requiredParams;
        }
    }
    return (argCount >= requiredParams) && (isVariadic || argCount <= maxParams);
}

namespace
{
struct FallbackCandidateRank
{
    bool arityMatches = false;
    bool exactParamCount = false;
    uint32_t arityDiff = 0;
    uint32_t viableArgs = 0;
    uint32_t incompatibleArgs = 0;

    auto operator<=>(const FallbackCandidateRank& other) const noexcept
    {
        if (auto c = arityMatches <=> other.arityMatches; c != 0)
        {
            return c;
        }
        if (auto c = exactParamCount <=> other.exactParamCount; c != 0)
        {
            return c;
        }
        if (auto c = other.arityDiff <=> arityDiff; c != 0)
        {
            return c;
        }
        if (auto c = viableArgs <=> other.viableArgs; c != 0)
        {
            return c;
        }
        return other.incompatibleArgs <=> incompatibleArgs;
    }
};

FallbackCandidateRank RankCandidateFallback(const FunctionSignature& sig, const std::vector<std::string>& argTypes,
                                            const SymbolTable& symbolTable)
{
    const uint32_t argCount = static_cast<uint32_t>(argTypes.size());
    FallbackCandidateRank rank;
    rank.arityMatches = MatchesCallArity(sig, argCount);
    rank.exactParamCount = (argCount == sig.parameters.size());
    rank.arityDiff =
        static_cast<uint32_t>(std::abs(static_cast<int>(argCount) - static_cast<int>(sig.parameters.size())));

    for (size_t i = 0; i < argTypes.size() && i < sig.parameters.size(); ++i)
    {
        if (!argTypes[i].empty())
        {
            const auto conv = EvaluateArgumentConversion(argTypes[i], sig.parameters[i], symbolTable);
            if (conv.IsViable())
            {
                ++rank.viableArgs;
            }
            else
            {
                ++rank.incompatibleArgs;
            }
        }
    }
    return rank;
}
} // namespace

const Symbol* FindBestFallbackOverload(const std::vector<Symbol>& candidates, const std::vector<std::string>& argTypes,
                                       const SymbolTable& symbolTable)
{
    const Symbol* best = nullptr;
    std::optional<FallbackCandidateRank> bestRank;
    for (const auto& sym : candidates)
    {
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
        {
            continue;
        }
        const auto& sig = std::get<FunctionSignature>(sym.signature);
        const auto rank = RankCandidateFallback(sig, argTypes, symbolTable);
        if (!bestRank || rank > *bestRank)
        {
            bestRank = rank;
            best = &sym;
        }
    }
    return best;
}

} // namespace angel_lsp::analysis
