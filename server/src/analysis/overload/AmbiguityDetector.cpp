#include "analysis/overload/AmbiguityDetector.h"
#include "analysis/overload/ConversionRankingEngine.h"
#include "analysis/overload/OverloadTypeConversions.h"

#include <algorithm>

namespace angel_lsp::analysis
{

bool HasSameParameterList(const Symbol& left, const Symbol& right)
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
        if (NormalizeType(a.parameters[i].typeName) != NormalizeType(b.parameters[i].typeName))
        {
            return false;
        }
        if (a.parameters[i].modifier != b.parameters[i].modifier)
        {
            return false;
        }
    }
    return true;
}

bool HasSameSignature(const Symbol& left, const Symbol& right)
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

    if (NormalizeType(a.returnType) != NormalizeType(b.returnType))
    {
        return false;
    }

    for (size_t i = 0; i < a.parameters.size(); ++i)
    {
        if (NormalizeType(a.parameters[i].typeName) != NormalizeType(b.parameters[i].typeName))
        {
            return false;
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
struct CandidateConversions
{
    std::vector<ArgumentConversion> conversions;
    std::vector<int> costVector;
    int totalScore = 0;
};

std::optional<CandidateConversions> ScoreCandidateArguments(const FunctionSignature& sig,
                                                            const std::vector<std::string>& argumentTypes,
                                                            const SymbolTable& symbolTable,
                                                            const std::vector<bool>& argIsLValue)
{
    CandidateConversions scores;
    scores.conversions.reserve(argumentTypes.size());
    scores.costVector.reserve(argumentTypes.size());
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
            scores.costVector.push_back(conv.legacyScore);
            scores.totalScore += conv.legacyScore;
            scores.conversions.push_back(conv);
        }
        else
        {
            ArgumentConversion varargConv{ConversionRank::StandardConv, 220, 0, false, 10};
            scores.costVector.push_back(varargConv.legacyScore);
            scores.totalScore += varargConv.legacyScore;
            scores.conversions.push_back(varargConv);
        }
    }
    return scores;
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

    auto scores = ScoreCandidateArguments(sig, argumentTypes, symbolTable, argIsLValue);
    if (!scores)
    {
        return std::nullopt;
    }

    int defaultArgs = 0;
    if (argCount < sig.parameters.size())
    {
        defaultArgs = static_cast<int>(sig.parameters.size() - argCount);
        scores->totalScore += defaultArgs * 1;
    }

    return EvaluatedCandidate{&sym, std::move(scores->conversions), std::move(scores->costVector), defaultArgs,
                              scores->totalScore};
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
    const bool hasLosslessCandidate = std::any_of(evaluated.begin(), evaluated.end(), [](const auto& cand) {
        return std::none_of(cand.conversions.begin(), cand.conversions.end(), [](const auto& c) { return c.isLossy; });
    });

    std::vector<EvaluatedCandidate> nonDominated;
    for (const auto& cand : evaluated)
    {
        if (hasLosslessCandidate &&
            std::any_of(cand.conversions.begin(), cand.conversions.end(), [](const auto& c) { return c.isLossy; }))
        {
            continue;
        }
        bool dominated = false;
        for (const auto& other : evaluated)
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
    return nonDominated;
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
                                                    const std::string bare = NormalizeType(argType);
                                                    return bare.empty() || bare == "auto";
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
