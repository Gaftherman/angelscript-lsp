#pragma once

#include "analysis/OverloadResolver.h"
#include "analysis/SymbolTable.h"

#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Evaluated candidate representation used in Pareto dominance and ambiguity calculation.
 */
struct EvaluatedCandidate
{
    const Symbol* symbol = nullptr;
    std::vector<ArgumentConversion> conversions;
    int defaultArgs = 0;
};

/**
 * @brief Function parameter count boundaries.
 */
struct ArityInfo
{
    uint32_t requiredParams = 0;
    uint32_t maxParams = 0;
    bool isVariadic = false;
};

/**
 * @brief Inspects parameter list of a function signature to determine arity boundaries.
 * @param[in] sig Function signature.
 * @return ArityInfo structure.
 */
ArityInfo InspectFunctionArity(const FunctionSignature& sig);

/**
 * @brief Checks if argument count is compatible with the given arity boundaries.
 * @param[in] arity Arity boundaries.
 * @param[in] argCount Passed argument count.
 * @return True if compatible.
 */
bool IsArityCompatible(const ArityInfo& arity, uint32_t argCount);

/**
 * @brief Checks if candidate a is strictly better than candidate b by Pareto dominance.
 * @param[in] a First candidate.
 * @param[in] b Second candidate.
 * @return True if a strictly dominates b.
 */
bool IsStrictlyBetter(const EvaluatedCandidate& a, const EvaluatedCandidate& b);

/**
 * @brief Filters a candidate set, retaining only non-dominated (Pareto-optimal) candidates.
 * @param[in] evaluated Full list of evaluated viable candidates.
 * @return Non-dominated candidates.
 */
std::vector<EvaluatedCandidate> FilterNonDominatedCandidates(const std::vector<EvaluatedCandidate>& evaluated);

/**
 * @brief Evaluates whether a set of non-dominated candidates represents an ambiguous call.
 * @param[in] nonDominated Pareto-optimal candidates.
 * @param[in] argumentTypes Deduced argument types.
 * @return True if ambiguity exists.
 */
bool CheckOverloadAmbiguity(const std::vector<EvaluatedCandidate>& nonDominated,
                            const std::vector<std::string>& argumentTypes);

/**
 * @brief Scores and evaluates a candidate symbol against given argument types.
 * @param[in] sym Candidate function symbol.
 * @param[in] argumentTypes Deduced argument types.
 * @param[in] symbolTable Symbol table.
 * @param[in] argIsLValue L-value flags.
 * @return EvaluatedCandidate if viable, otherwise std::nullopt.
 */
std::optional<EvaluatedCandidate> EvaluateCandidate(const Symbol& sym, const std::vector<std::string>& argumentTypes,
                                                    const SymbolTable& symbolTable,
                                                    const std::vector<bool>& argIsLValue);

/**
 * @brief Checks if two function symbols declare the identical signature (name, params, return).
 * @param[in] left First symbol.
 * @param[in] right Second symbol.
 * @return True if identical signature.
 */
bool HasSameSignature(const Symbol& left, const Symbol& right);

} // namespace angel_lsp::analysis
