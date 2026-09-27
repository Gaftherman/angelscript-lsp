#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/overload/AmbiguityDetector.h"
#include "analysis/overload/ConversionRankingEngine.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "utils/MultiFileLogger.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

void OverloadResolver::addFunction(const FunctionSymbol& sym)
{
    if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
    {
        const auto& sig = sym.GetFunction();
        const auto arity = InspectFunctionArity(sig);
        for (size_t a = arity.requiredParams; a <= arity.maxParams; ++a)
        {
            functionIndex_[sym.name][a].push_back(sym);
        }
        if (arity.isVariadic)
        {
            for (size_t a = arity.maxParams + 1; a <= std::max<size_t>(arity.maxParams + 16, 20); ++a)
            {
                functionIndex_[sym.name][a].push_back(sym);
            }
        }
    }
}

void OverloadResolver::indexCandidates(std::span<const FunctionSymbol> symbols)
{
    for (const auto& sym : symbols)
    {
        addFunction(sym);
    }
}

void OverloadResolver::clear()
{
    functionIndex_.clear();
}

std::span<const FunctionSymbol> OverloadResolver::findCandidates(std::string_view name, size_t argCount) const
{
    const auto itName = functionIndex_.find(std::string(name));
    if (itName != functionIndex_.end())
    {
        const auto itArity = itName->second.find(argCount);
        if (itArity != itName->second.end())
        {
            return itArity->second;
        }
    }
    return {};
}

namespace
{
std::string FormatCandidateSignature(const Symbol& sym)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return sym.name;
    }
    const auto& sig = sym.GetFunction();
    std::string res = sym.name + "(";
    for (size_t i = 0; i < sig.parameters.size(); ++i)
    {
        if (i > 0)
        {
            res += ", ";
        }
        res += sig.parameters[i].typeName;
        if (!sig.parameters[i].name.empty())
        {
            res += " " + sig.parameters[i].name;
        }
    }
    res += ")";
    return res;
}

std::string FindRejectionReason(const Symbol& sym, const std::vector<std::string>& argumentTypes)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return "Not a function";
    }
    const auto& sig = sym.GetFunction();
    const auto arity = InspectFunctionArity(sig);
    if (!IsArityCompatible(arity, static_cast<uint32_t>(argumentTypes.size())))
    {
        return "Arity mismatch: expected " + std::to_string(arity.requiredParams) + ".." +
               std::to_string(arity.maxParams) + ", got " + std::to_string(argumentTypes.size());
    }
    for (size_t i = 0; i < argumentTypes.size() && i < sig.parameters.size(); ++i)
    {
        if (AreIncompatibleTemplateTypes(argumentTypes[i], sig.parameters[i].typeName))
        {
            return "Template arg mismatch: '" + argumentTypes[i] + "' vs '" + sig.parameters[i].typeName + "'";
        }
    }
    return "Type conversion incompatible";
}

struct OverloadLogRequest
{
    std::span<const Symbol* const> candidates;
    const std::vector<std::string>& argumentTypes;
    const OverloadMatchResult& result;
    const std::vector<EvaluatedCandidate>& evaluated;
};

void LogOverloadTelemetry(const OverloadLogRequest& req)
{
    if (!utils::MultiFileLogger::Instance().IsInitialized() || req.candidates.empty())
    {
        return;
    }
    const std::string fnName = req.candidates.front() ? req.candidates.front()->name : "unknown";
    std::string argsStr;
    for (size_t i = 0; i < req.argumentTypes.size(); ++i)
    {
        if (i > 0)
        {
            argsStr += ", ";
        }
        argsStr += req.argumentTypes[i];
    }
    utils::MultiFileLogger::Instance().LogOverload(utils::MultiFileLogLevel::Info,
                                                   "Resolving '" + fnName + "' with args: (" + argsStr + ")");

    for (size_t i = 0; i < req.candidates.size(); ++i)
    {
        const Symbol* sym = req.candidates[i];
        if (!sym)
        {
            continue;
        }
        const std::string candSig = FormatCandidateSignature(*sym);
        const auto it = std::find_if(req.evaluated.begin(), req.evaluated.end(),
                                     [sym](const EvaluatedCandidate& e) { return e.symbol == sym; });
        if (it != req.evaluated.end())
        {
            utils::MultiFileLogger::Instance().LogOverload(
                utils::MultiFileLogLevel::Info, "  Candidate " + std::to_string(i + 1) + " '" + candSig +
                                                    "': ACCEPTED (Score=" + std::to_string(it->totalCost) + ")");
        }
        else
        {
            const std::string reason = FindRejectionReason(*sym, req.argumentTypes);
            utils::MultiFileLogger::Instance().LogOverload(utils::MultiFileLogLevel::Info,
                                                           "  Candidate " + std::to_string(i + 1) + " '" + candSig +
                                                               "': REJECTED (" + reason + ")");
        }
    }
    if (req.result.bestCandidate)
    {
        utils::MultiFileLogger::Instance().LogOverload(
            utils::MultiFileLogLevel::Info, "Winner: " + FormatCandidateSignature(*req.result.bestCandidate));
    }
    else
    {
        utils::MultiFileLogger::Instance().LogOverload(utils::MultiFileLogLevel::Info,
                                                       "Winner: None (no viable overload)");
    }
}
} // namespace

OverloadMatchResult ResolveBestOverload(std::span<const Symbol* const> candidates,
                                        const std::vector<std::string>& argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue)
{
    OverloadMatchResult result;
    std::vector<EvaluatedCandidate> evaluated;

    for (const Symbol* sym : candidates)
    {
        if (!sym)
        {
            continue;
        }
        if (auto cand = EvaluateCandidate(*sym, argumentTypes, symbolTable, argIsLValue))
        {
            result.viableCandidates.push_back(sym);
            evaluated.push_back(std::move(*cand));
        }
    }

    if (evaluated.empty())
    {
        const OverloadLogRequest logReq{candidates, argumentTypes, result, evaluated};
        LogOverloadTelemetry(logReq);
        return result;
    }

    if (evaluated.size() == 1)
    {
        result.bestCandidate = evaluated.front().symbol;
        result.bestScore = evaluated.front().totalCost;
        result.bestCostVector = std::move(evaluated.front().costVector);
        result.bestConversions = std::move(evaluated.front().conversions);
        const OverloadLogRequest logReq{candidates, argumentTypes, result, evaluated};
        LogOverloadTelemetry(logReq);
        return result;
    }

    auto nonDominated = FilterNonDominatedCandidates(evaluated);
    if (nonDominated.empty())
    {
        const OverloadLogRequest logReq{candidates, argumentTypes, result, evaluated};
        LogOverloadTelemetry(logReq);
        return result;
    }

    result.isAmbiguous = CheckOverloadAmbiguity(nonDominated, argumentTypes);
    if (result.isAmbiguous)
    {
        result.bestCandidate = nullptr;
    }
    else
    {
        result.bestCandidate = nonDominated.front().symbol;
        result.bestScore = nonDominated.front().totalCost;
        result.bestCostVector = nonDominated.front().costVector;
        result.bestConversions = nonDominated.front().conversions;
    }

    const OverloadLogRequest logReq{candidates, argumentTypes, result, evaluated};
    LogOverloadTelemetry(logReq);
    return result;
}

OverloadMatchResult ResolveBestOverload(const std::vector<Symbol>& candidates,
                                        const std::vector<std::string>& argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue)
{
    std::vector<const Symbol*> ptrs;
    ptrs.reserve(candidates.size());
    for (const auto& sym : candidates)
    {
        ptrs.push_back(&sym);
    }
    return ResolveBestOverload(ptrs, argumentTypes, symbolTable, argIsLValue);
}

} // namespace angel_lsp::analysis
