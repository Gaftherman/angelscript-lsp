#include "analysis/overload/AmbiguityDetector.h"
#include "analysis/ExpressionTypeCache.h"
#include "analysis/TypeSanitization.h"
#include "analysis/overload/ConversionRankingEngine.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "parser/Keywords.h"

#include <algorithm>

namespace angel_lsp::analysis
{

bool HasSameParameterList(const Symbol& left, const Symbol& right)
{
    return HasSameParameterList(left, right, {});
}

bool HasSameSignature(const Symbol& left, const Symbol& right)
{
    return HasSameSignature(left, right, {});
}

bool HasSameParameterList(const Symbol& left, const Symbol& right, std::string_view arrayTypeName)
{
    if (!std::holds_alternative<FunctionSignature>(left.signature) ||
        !std::holds_alternative<FunctionSignature>(right.signature))
    {
        return false;
    }

    const auto& a = left.GetFunction();
    const auto& b = right.GetFunction();

    if (a.parameters.size() != b.parameters.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.parameters.size(); ++i)
    {
        const auto& pa = a.parameters[i].typeName;
        const auto& pb = b.parameters[i].typeName;
        if (pa != pb)
        {
            if (arrayTypeName.empty() || NormalizeType(pa, arrayTypeName) != NormalizeType(pb, arrayTypeName))
            {
                return false;
            }
        }
        if (a.parameters[i].modifier != b.parameters[i].modifier)
        {
            return false;
        }
    }
    return true;
}

bool HasSameSignature(const Symbol& left, const Symbol& right, std::string_view arrayTypeName)
{
    if (!std::holds_alternative<FunctionSignature>(left.signature) ||
        !std::holds_alternative<FunctionSignature>(right.signature))
    {
        return false;
    }

    const auto& a = left.GetFunction();
    const auto& b = right.GetFunction();

    if (left.qualifiedName != right.qualifiedName)
    {
        return false;
    }

    if (a.parameters.size() != b.parameters.size())
    {
        return false;
    }

    if (a.returnType != b.returnType)
    {
        if (arrayTypeName.empty() ||
            NormalizeType(a.returnType, arrayTypeName) != NormalizeType(b.returnType, arrayTypeName))
        {
            return false;
        }
    }

    for (size_t i = 0; i < a.parameters.size(); ++i)
    {
        const auto& pa = a.parameters[i].typeName;
        const auto& pb = b.parameters[i].typeName;
        if (pa != pb)
        {
            if (arrayTypeName.empty() || NormalizeType(pa, arrayTypeName) != NormalizeType(pb, arrayTypeName))
            {
                return false;
            }
        }
        if (a.parameters[i].modifier != b.parameters[i].modifier)
        {
            return false;
        }
    }

    return true;
}

ArityInfo InspectFunctionArity(const FunctionSignature& sig)
{
    ArityInfo info;
    for (const auto& param : sig.parameters)
    {
        if (param.rawText.find("...") != std::string::npos || param.typeName.find("...") != std::string::npos)
        {
            info.isVariadic = true;
            continue;
        }
        ++info.maxParams;
        if (param.defaultValue.empty() && param.rawText.find('=') == std::string::npos)
        {
            ++info.requiredParams;
        }
    }
    return info;
}

bool IsArityCompatible(const ArityInfo& arity, uint32_t argCount)
{
    if (!arity.isVariadic)
    {
        return argCount >= arity.requiredParams && argCount <= arity.maxParams;
    }
    return argCount >= arity.requiredParams;
}

namespace
{
std::optional<std::vector<ArgumentConversion>>
EvaluateCandidateConversions(const FunctionSignature& sig, const std::vector<std::string>& argumentTypes,
                             const SymbolTable& symbolTable, const std::vector<bool>& argIsLValue)
{
    std::vector<ArgumentConversion> conversions;
    conversions.reserve(argumentTypes.size());
    for (uint32_t i = 0; i < argumentTypes.size(); ++i)
    {
        if (i < sig.parameters.size())
        {
            const bool isLVal = i < argIsLValue.size() ? argIsLValue[i] : true;
            ArgumentConversion conv =
                EvaluateArgumentConversion(argumentTypes[i], sig.parameters[i], symbolTable, isLVal);
            if (!conv.IsViable())
            {
                return std::nullopt;
            }
            conversions.push_back(conv);
        }
        else
        {
            ArgumentConversion varargConv{ConversionRank::StandardConv, 220, 0, false, 10};
            conversions.push_back(varargConv);
        }
    }
    return conversions;
}
} // namespace

std::optional<EvaluatedCandidate> EvaluateCandidate(const Symbol& sym, const std::vector<std::string>& argumentTypes,
                                                    const SymbolTable& symbolTable,
                                                    const std::vector<bool>& argIsLValue)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return std::nullopt;
    }

    const auto& sig = sym.GetFunction();
    const uint32_t argCount = static_cast<uint32_t>(argumentTypes.size());
    const auto arity = InspectFunctionArity(sig);
    if (!IsArityCompatible(arity, argCount))
    {
        return std::nullopt;
    }

    auto conversions = EvaluateCandidateConversions(sig, argumentTypes, symbolTable, argIsLValue);
    if (!conversions)
    {
        return std::nullopt;
    }

    int defaultArgs = 0;
    if (argCount < sig.parameters.size())
    {
        defaultArgs = static_cast<int>(sig.parameters.size() - argCount);
    }

    return EvaluatedCandidate{&sym, std::move(*conversions), defaultArgs};
}

bool IsStrictlyBetter(const EvaluatedCandidate& a, const EvaluatedCandidate& b)
{
    if (a.conversions.size() != b.conversions.size())
    {
        return false;
    }
    bool hasStrictlyBetterArg = false;
    for (size_t i = 0; i < a.conversions.size(); ++i)
    {
        if (a.conversions[i] > b.conversions[i])
        {
            return false;
        }
        if (a.conversions[i] < b.conversions[i])
        {
            hasStrictlyBetterArg = true;
        }
    }
    if (hasStrictlyBetterArg)
    {
        return true;
    }
    return a.defaultArgs < b.defaultArgs;
}

std::vector<EvaluatedCandidate> FilterNonDominatedCandidates(const std::vector<EvaluatedCandidate>& evaluated)
{
    if (evaluated.empty())
    {
        return {};
    }

    uint32_t minCost = std::numeric_limits<uint32_t>::max();
    for (const auto& cand : evaluated)
    {
        uint32_t cost = 0;
        for (const auto& c : cand.conversions)
        {
            cost += static_cast<uint32_t>(c.tier);
        }
        if (cost < minCost)
        {
            minCost = cost;
        }
    }

    std::vector<EvaluatedCandidate> bestCandidates;
    for (const auto& cand : evaluated)
    {
        uint32_t cost = 0;
        for (const auto& c : cand.conversions)
        {
            cost += static_cast<uint32_t>(c.tier);
        }
        if (cost == minCost)
        {
            bestCandidates.push_back(cand);
        }
    }

    if (bestCandidates.size() > 1)
    {
        std::vector<EvaluatedCandidate> nonDominated;
        for (const auto& cand : bestCandidates)
        {
            bool dominated = false;
            for (const auto& other : bestCandidates)
            {
                if (&cand != &other && IsStrictlyBetter(other, cand))
                {
                    dominated = true;
                    break;
                }
            }
            if (!dominated)
            {
                nonDominated.push_back(cand);
            }
        }
        bestCandidates = std::move(nonDominated);
    }

    return bestCandidates;
}

bool CheckOverloadAmbiguity(const std::vector<EvaluatedCandidate>& nonDominated,
                            const std::vector<std::string>& argumentTypes)
{
    if (nonDominated.size() <= 1)
    {
        return false;
    }

    const bool anyArgumentUnknown = std::any_of(argumentTypes.begin(), argumentTypes.end(),
                                                [](const std::string& argType)
                                                {
                                                    const std::string_view bare = CleanBaseTypeView(argType);
                                                    return bare.empty() || bare == parser::keywords::Auto ||
                                                           ExpressionTypeCache::IsPoisonType(bare);
                                                });
    if (anyArgumentUnknown)
    {
        return false;
    }

    for (size_t i = 1; i < nonDominated.size(); ++i)
    {
        if (!HasSameSignature(*nonDominated.front().symbol, *nonDominated[i].symbol))
        {
            return true;
        }
    }
    return false;
}

} // namespace angel_lsp::analysis
