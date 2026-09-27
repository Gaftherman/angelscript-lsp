#include "analysis/overload/ConversionRankingEngine.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/overload/OverloadTypeConversions.h"

#include <algorithm>
#include <optional>
#include <string>

namespace angel_lsp::analysis
{
namespace
{
std::optional<ArgumentConversion> EvaluateSpecialArgumentMatch(const std::string& argType,
                                                               const ParameterInformation& param)
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
        if (IsContainerParameter(param))
        {
            return ArgumentConversion{ConversionRank::Exact, 0, 0, false,
                                      static_cast<int>(OverloadMatchPenalty::Exact)};
        }
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }
    return std::nullopt;
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
    return ArgumentConversion{ConversionRank::StandardConv, 10, 0, false,
                              static_cast<int>(OverloadMatchPenalty::Narrowing)};
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

std::optional<ArgumentConversion> EvaluatePrimitiveOrEnumConversion(const MatchContext& ctx)
{
    const auto namesAnEnum = [&ctx](const std::string& typeName)
    {
        const auto symbols = ctx.table.FindSymbolsPtr(typeName);
        return symbols && std::any_of(symbols->begin(), symbols->end(),
                                      [](const Symbol& sym) { return sym.type == SymbolType::Enum; });
    };

    if (IsIntegerType(ctx.cleanParam) && namesAnEnum(ctx.cleanArg))
    {
        return ArgumentConversion{ConversionRank::Promotion, 0, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Widening)};
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
    if (IsPrimitiveNarrowing(ctx.cleanArg, ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::StandardConv, 10, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Narrowing)};
    }
    return std::nullopt;
}

ArgumentConversion EvaluateCustomOrUnresolvedConversion(const MatchContext& ctx)
{
    if (HasUserConversion(ctx.cleanArg, ctx.cleanParam, ctx.table))
    {
        return ArgumentConversion{ConversionRank::UserDefined, 0, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::UserDefined)};
    }

    if (AreIncompatibleTemplateTypes(ctx.cleanArg, ctx.cleanParam))
    {
        return ArgumentConversion{ConversionRank::Incompatible, 255, 0, false,
                                  static_cast<int>(OverloadMatchPenalty::Incompatible)};
    }

    const auto isNamedAndUnresolved = [&ctx](const std::string& typeName)
    {
        if (typeName.empty() || IsCorePrimitive(typeName) || typeName == "string")
        {
            return false;
        }
        const std::string owner = MemberOwnerType(typeName);
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
    if (auto specialScore = EvaluateSpecialArgumentMatch(argType, param))
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

} // namespace angel_lsp::analysis
