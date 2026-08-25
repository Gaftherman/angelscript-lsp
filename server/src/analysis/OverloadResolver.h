#pragma once

#include "analysis/SymbolTable.h"
#include <string>
#include <vector>
#include <optional>

namespace angel_lsp::analysis
{
    /**
     * @brief Match penalty categories for argument-to-parameter type conversion.
     */
    enum class OverloadMatchPenalty : int
    {
        Exact = 0,               ///< Identical types (int -> int, Player@ -> Player@)
        ConstRef = 1,            ///< Const/ref conversion (T -> const T&in, T@ -> const T@)
        Inheritance = 2,         ///< Derived -> Base / Interface (Derived@ -> Base@)
        Widening = 3,            ///< Safe numeric promotion (int8 -> int32, float -> double)
        Narrowing = 4,           ///< Lossy conversion (double -> float, int -> bool, float -> int)
        UserDefined = 5,         ///< opImplConv / single-arg converting constructor
        Incompatible = 999       ///< No viable conversion
    };

    /**
     * @brief Result of evaluating function overload candidates.
     */
    struct OverloadMatchResult
    {
        const Symbol *bestCandidate = nullptr; ///< Best matching function candidate, or nullptr if none
        int bestScore = 999999;                ///< Cumulative penalty score of best candidate
        bool isAmbiguous = false;              ///< True if two or more candidates tied for best score
        std::vector<const Symbol *> viableCandidates; ///< All viable candidates with finite penalty scores
    };

    /**
     * @brief Computes the penalty score for passing an argument of type argType to parameter param.
     * @param argType Cleaned type of the argument expression.
     * @param param Target parameter specification.
     * @param symbolTable Symbol table for hierarchy and conversion lookups.
     * @return Penalty score integer (0 = exact, >= 999 = incompatible).
     */
    int ScoreArgumentMatch(
        const std::string &argType,
        const ParameterInformation &param,
        const SymbolTable &symbolTable);

    /**
     * @brief Selects the optimal function/method symbol from a candidate overload set.
     * @param candidates Set of candidate function symbols.
     * @param argumentTypes Deduced argument types for each argument expression.
     * @param symbolTable Symbol table for hierarchy and conversion lookups.
     * @return OverloadMatchResult containing the best candidate and match metrics.
     */
    OverloadMatchResult ResolveBestOverload(
        const std::vector<Symbol> &candidates,
        const std::vector<std::string> &argumentTypes,
        const SymbolTable &symbolTable);

    /**
     * @brief Checks whether fromType can be widened to toType without precision loss.
     * @param fromType Source primitive type.
     * @param toType Destination primitive type.
     * @return True if widening conversion is valid.
     */
    bool IsPrimitiveWidening(const std::string &fromType, const std::string &toType);

    /**
     * @brief Checks whether fromType to toType is a valid primitive narrowing or cross conversion.
     * @param fromType Source primitive type.
     * @param toType Destination primitive type.
     * @return True if narrowing conversion is valid.
     */
    bool IsPrimitiveNarrowing(const std::string &fromType, const std::string &toType);
}
