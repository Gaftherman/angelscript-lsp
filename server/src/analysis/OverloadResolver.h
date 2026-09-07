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
        Widening = 3,            ///< Safe promotion within one kind (int8 -> int32, float -> double)

        Narrowing = 4,           ///< Lossy conversion within one kind (double -> float, float -> int)

        /**
         * @brief Integer to integer where the signedness differs (int -> uint64, int -> uint8).
         *
         * A rank of its own, and size does not refine it. Measured: for an `int` argument, `int64`
         * beats `uint64` and even `int16` beats `uint64`, while `uint8` against `uint64` is
         * "Multiple matching signatures" - so any signedness change ranks below every conversion
         * that keeps it, and two of them tie however far apart their widths are.
         *
         * Without this rank int -> int64 and int -> uint64 both scored Widening, which is why a
         * real script's `Math.min( 255, numBubbles )` was reported as an ambiguous call: the Sven
         * Co-op stub declares min for float, int64 and uint64 and for no smaller integer.
         */
        SignednessChange = 5,

        /**
         * @brief Safe promotion that also crosses from integer to floating point (int -> double).
         *
         * Ranked below every integer conversion, including a narrowing one: measured, an `int`
         * argument picks `int16` over `double` and `int8` over `float`, and picks `uint64` over
         * `float` even though that changes signedness. Two floating point candidates tie with each
         * other - `double` against `float` is "Multiple matching signatures".
         *
         * The distinction from same-kind widening was introduced for the standard dictionary,
         * which declares both `set(const string&in, const int64&in)` and `set(const string&in,
         * const double&in)`: scoring int -> int64 and int -> double identically made every
         * `dict.set("k", 95)` report as ambiguous. That still resolves, now by two ranks.
         */
        WideningAcrossKind = 6,

        UserDefined = 7,         ///< opImplConv / single-arg converting constructor

        /**
         * @brief Neither type has a declaration this analyzer can read, so nothing can be ruled out.
         *
         * `CBasePlayer@` passed where `CBaseEntity@` is expected is an upcast in every Sven Co-op
         * script, and both classes are registered by the engine in C++ - so the hierarchy walk
         * finds no relation and, scored Incompatible, the call was reported as having no matching
         * signature. Thirteen of the corpus findings were exactly this.
         *
         * Ranked last among the viable scores so it never displaces a match the analyzer can
         * actually see: an overload set with one visible match still picks that one. It only keeps
         * a call from being *rejected* on the strength of declarations that are not there.
         */
        UnknownTypes = 8,

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
    /**
     * @brief True when two declarations take the same parameters, whoever declares them.
     *
     * The internal HasSameSignature also compares the qualified name, which is right for its own
     * question - "the same function arriving twice", from two stubs describing one library. This is
     * a different question: a method redeclared in a SUBCLASS. A member lookup walks the hierarchy
     * and finds the base's declaration alongside the derived one that overrides it, and those two
     * are one method rather than a choice between two.
     */
    bool HasSameParameterList(const Symbol &left, const Symbol &right);

    bool IsPrimitiveWidening(const std::string &fromType, const std::string &toType);

    /**
     * @brief Checks whether fromType to toType is a valid primitive narrowing or cross conversion.
     * @param fromType Source primitive type.
     * @param toType Destination primitive type.
     * @return True if narrowing conversion is valid.
     */
    bool IsPrimitiveNarrowing(const std::string &fromType, const std::string &toType);
}
