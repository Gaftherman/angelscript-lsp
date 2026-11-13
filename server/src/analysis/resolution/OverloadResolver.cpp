#include "analysis/resolution/OverloadResolver.h"

#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/ConversionRankingEngine.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
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
    const auto itName = functionIndex_.find(name);
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

static std::string FormatCandidateSignature(const Symbol& sym)
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

static std::string FindRejectionReason(const Symbol& sym, std::span<const std::string_view> argumentTypes)
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
            return "Template arg mismatch: '" + std::string(argumentTypes[i]) + "' vs '" + sig.parameters[i].typeName +
                   "'";
        }
    }
    return "Type conversion incompatible";
}

static void LogOverloadTelemetry(const OverloadLogRequest& req)
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
        argsStr.append(req.argumentTypes[i]);
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
            utils::MultiFileLogger::Instance().LogOverload(utils::MultiFileLogLevel::Info,
                                                           "  Candidate " + std::to_string(i + 1) + " '" + candSig +
                                                               "': ACCEPTED (" +
                                                               std::to_string(it->conversions.size()) + " args)");
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

static const Symbol* SelectPriorityCandidate(const std::vector<EvaluatedCandidate>& nonDominated)
{
    if (nonDominated.empty())
    {
        return nullptr;
    }
    const EvaluatedCandidate* best = &nonDominated.front();
    uint32_t bestTierScore = 0;
    for (const auto& c : best->conversions)
    {
        bestTierScore += static_cast<uint32_t>(c.tier);
    }
    for (size_t i = 1; i < nonDominated.size(); ++i)
    {
        uint32_t tierScore = 0;
        for (const auto& c : nonDominated[i].conversions)
        {
            tierScore += static_cast<uint32_t>(c.tier);
        }
        if (tierScore < bestTierScore ||
            (tierScore == bestTierScore && nonDominated[i].defaultArgs < best->defaultArgs))
        {
            best = &nonDominated[i];
            bestTierScore = tierScore;
        }
    }
    return best->symbol;
}

static void PopulateWinner(OverloadMatchResult& result, EvaluatedCandidate cand)
{
    result.bestCandidate = cand.symbol;
    result.priorityCandidate = cand.symbol;
    result.bestScore = 0;
    result.bestCostVector.clear();
    result.bestCostVector.reserve(cand.conversions.size());
    for (const auto& c : cand.conversions)
    {
        result.bestCostVector.push_back(c.legacyScore);
        result.bestScore += c.legacyScore;
    }
    result.bestConversions = std::move(cand.conversions);
}

OverloadMatchResult ResolveBestOverload(std::span<const Symbol* const> candidates,
                                        std::span<const std::string_view> argumentTypes, const SymbolTable& symbolTable,
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
        PopulateWinner(result, std::move(evaluated.front()));
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
        result.priorityCandidate = SelectPriorityCandidate(nonDominated);
    }
    else
    {
        PopulateWinner(result, std::move(nonDominated.front()));
    }

    const OverloadLogRequest logReq{candidates, argumentTypes, result, evaluated};
    LogOverloadTelemetry(logReq);
    return result;
}

static std::vector<const Symbol*> CandidatePtrs(const std::vector<Symbol>& candidates)
{
    std::vector<const Symbol*> ptrs;
    ptrs.reserve(candidates.size());
    for (const auto& sym : candidates)
    {
        ptrs.push_back(&sym);
    }
    return ptrs;
}

static std::vector<std::string_view> ToViews(std::span<const std::string> argumentTypes)
{
    std::vector<std::string_view> views;
    views.reserve(argumentTypes.size());
    for (const auto& a : argumentTypes)
    {
        views.emplace_back(a);
    }
    return views;
}

OverloadMatchResult ResolveBestOverload(std::span<const Symbol* const> candidates,
                                        std::initializer_list<std::string_view> argumentTypes,
                                        const SymbolTable& symbolTable, const std::vector<bool>& argIsLValue)
{
    const std::vector<std::string_view> args(argumentTypes);
    return ResolveBestOverload(candidates, std::span<const std::string_view>(args), symbolTable, argIsLValue);
}

OverloadMatchResult ResolveBestOverload(std::span<const Symbol* const> candidates,
                                        std::span<const std::string> argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue)
{
    const auto views = ToViews(argumentTypes);
    return ResolveBestOverload(candidates, std::span<const std::string_view>(views), symbolTable, argIsLValue);
}

OverloadMatchResult ResolveBestOverload(std::span<const Symbol* const> candidates,
                                        const std::vector<std::string>& argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue)
{
    return ResolveBestOverload(candidates, std::span<const std::string>(argumentTypes), symbolTable, argIsLValue);
}

OverloadMatchResult ResolveBestOverload(const std::vector<Symbol>& candidates,
                                        std::span<const std::string_view> argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue)
{
    return ResolveBestOverload(CandidatePtrs(candidates), argumentTypes, symbolTable, argIsLValue);
}

OverloadMatchResult ResolveBestOverload(const std::vector<Symbol>& candidates,
                                        std::initializer_list<std::string_view> argumentTypes,
                                        const SymbolTable& symbolTable, const std::vector<bool>& argIsLValue)
{
    const std::vector<std::string_view> args(argumentTypes);
    return ResolveBestOverload(CandidatePtrs(candidates), std::span<const std::string_view>(args), symbolTable,
                               argIsLValue);
}

OverloadMatchResult ResolveBestOverload(const std::vector<Symbol>& candidates,
                                        std::span<const std::string> argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue)
{
    return ResolveBestOverload(CandidatePtrs(candidates), argumentTypes, symbolTable, argIsLValue);
}

OverloadMatchResult ResolveBestOverload(const std::vector<Symbol>& candidates,
                                        const std::vector<std::string>& argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue)
{
    return ResolveBestOverload(CandidatePtrs(candidates), argumentTypes, symbolTable, argIsLValue);
}

} // namespace angel_lsp::analysis
