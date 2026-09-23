#pragma once

#include "analysis/SymbolTable.h"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Formal conversion rank hierarchy according to standard compiler design.
 */
enum class ConversionRank : uint8_t
{
    Exact = 0,        ///< Identity or const/reference adjustment with no data conversion
    Promotion = 1,    ///< Value-preserving promotion (widening within kind, e.g. int8 -> int32, float -> double)
    StandardConv = 2, ///< Standard language conversion (numeric cross-kind/narrowing, base/derived inheritance)
    UserDefined = 3,  ///< Explicit/implicit constructor or conversion method (opImplConv / opImplCast)
    Incompatible = 4  ///< No valid conversion exists
};

/**
 * @brief Formal argument-to-parameter type conversion representation.
 */
struct ArgumentConversion
{
    ConversionRank rank = ConversionRank::Incompatible;
    uint8_t subRank = 0;             ///< Secondary ordering within rank
    uint8_t inheritanceDistance = 0; ///< Distance in inheritance hierarchy (0 = exact / same class)
    bool isConstAdjustment = false;  ///< Const/reference qualification adjustment
    int legacyScore = 999;           ///< Legacy scalar score for backwards compatibility

    /**
     * @brief Checks if conversion is viable for candidate invocation.
     * @return True if conversion rank is not Incompatible.
     */
    [[nodiscard]] constexpr bool IsViable() const noexcept
    {
        return rank != ConversionRank::Incompatible;
    }

    /**
     * @brief Three-way lexicographical comparison between argument conversions.
     * @param[in] other Other conversion to compare against.
     * @return Strong ordering comparison result.
     */
    [[nodiscard]] auto operator<=>(const ArgumentConversion& other) const noexcept
    {
        if (auto cmp = rank <=> other.rank; cmp != 0)
        {
            return cmp;
        }
        if (auto cmp = inheritanceDistance <=> other.inheritanceDistance; cmp != 0)
        {
            return cmp;
        }
        if (auto cmp = isConstAdjustment <=> other.isConstAdjustment; cmp != 0)
        {
            return cmp;
        }
        return subRank <=> other.subRank;
    }

    [[nodiscard]] bool operator==(const ArgumentConversion& other) const noexcept = default;
};

/**
 * @brief Match penalty categories for argument-to-parameter type conversion (legacy compatibility).
 */
enum class OverloadMatchPenalty : int
{
    Exact = 0,       ///< Identical types (int -> int, Player@ -> Player@)
    ConstRef = 1,    ///< Const/ref conversion (T -> const T&in, T@ -> const T@)
    Inheritance = 2, ///< Derived -> Base / Interface (Derived@ -> Base@)
    Widening = 3,    ///< Safe promotion within one kind (int8 -> int32, float -> double)

    Narrowing = 4, ///< Lossy conversion within one kind (double -> float, float -> int)

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

    UserDefined = 7, ///< opImplConv / single-arg converting constructor

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

    /**
     * @brief R-value passed to an &out or mutable reference parameter.
     *
     * Penalized so that by-value or const-reference overloads win, while single-candidate
     * calls can still proceed to emit specialized l-value required diagnostics.
     */
    RValueToOutParam = 20,

    Incompatible = 999 ///< No viable conversion
};

/**
 * @brief Result of evaluating function overload candidates.
 */
struct OverloadMatchResult
{
    const Symbol* bestCandidate = nullptr;           ///< Best matching function candidate, or nullptr if none
    int bestScore = 999999;                          ///< Cumulative penalty score of best candidate (legacy)
    std::vector<int> bestCostVector;                 ///< Argument conversion penalty vector of best candidate (legacy)
    std::vector<ArgumentConversion> bestConversions; ///< Argument conversion ranks of best candidate
    bool isAmbiguous = false;                        ///< True if two or more candidates tied for best score
    std::vector<const Symbol*> viableCandidates;     ///< All viable candidates with finite penalty scores
};

using FunctionSymbol = Symbol;

/**
 * @brief Arity-partitioned overload index for O(1) candidate lookup.
 */
class OverloadResolver
{
  public:
    OverloadResolver() = default;

    /**
     * @brief Indexes a function candidate by its name and parameter count.
     * @param[in] sym The function symbol to index.
     */
    void addFunction(const FunctionSymbol& sym);

    /**
     * @brief Populates index from a collection of candidate symbols.
     * @param[in] symbols Candidate symbol collection.
     */
    void indexCandidates(std::span<const FunctionSymbol> symbols);

    /**
     * @brief Clears all indexed functions.
     */
    void clear();

    /**
     * @brief Finds candidates matching the given name and parameter count in O(1).
     * @param[in] name Function identifier.
     * @param[in] argCount Parameter count.
     * @return Span of matching candidate symbols.
     */
    [[nodiscard]] std::span<const FunctionSymbol> findCandidates(std::string_view name, size_t argCount) const;

    /**
     * @brief Direct accessor to the internal function index map.
     * @return Const reference to internal map.
     */
    [[nodiscard]] const std::unordered_map<std::string, std::unordered_map<size_t, std::vector<FunctionSymbol>>>&
    getFunctionIndex() const
    {
        return functionIndex_;
    }

  private:
    // Key: Function Identifier -> Map: Parameter Arity -> List of Candidates
    std::unordered_map<std::string, std::unordered_map<size_t, std::vector<FunctionSymbol>>> functionIndex_;
};

/**
 * @brief Evaluates formal conversion rank for passing an argument of type argType to parameter param.
 * @param[in] argType Cleaned type of the argument expression.
 * @param[in] param Target parameter specification.
 * @param[in] symbolTable Symbol table for hierarchy and conversion lookups.
 * @param[in] argIsLValue Whether argument is an L-value.
 * @return Formal ArgumentConversion structure.
 */
ArgumentConversion EvaluateArgumentConversion(const std::string& argType, const ParameterInformation& param,
                                              const SymbolTable& symbolTable, bool argIsLValue = true);

/**
 * @brief Computes the penalty score for passing an argument of type argType to parameter param.
 * @param argType Cleaned type of the argument expression.
 * @param param Target parameter specification.
 * @param symbolTable Symbol table for hierarchy and conversion lookups.
 * @param argIsLValue Optional flag indicating if argument is an L-value.
 * @return Penalty score integer (0 = exact, >= 999 = incompatible).
 */
int ScoreArgumentMatch(const std::string& argType, const ParameterInformation& param, const SymbolTable& symbolTable,
                       bool argIsLValue = true);

/**
 * @brief Selects the optimal function/method symbol from a candidate overload set.
 * @param candidates Set of candidate function symbols.
 * @param argumentTypes Deduced argument types for each argument expression.
 * @param symbolTable Symbol table for hierarchy and conversion lookups.
 * @param argIsLValue Optional flags indicating whether each argument expression is an L-value.
 * @return OverloadMatchResult containing the best candidate and match metrics.
 */
OverloadMatchResult ResolveBestOverload(const std::vector<Symbol>& candidates,
                                        const std::vector<std::string>& argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue = {});

/**
 * @brief Selects the optimal function/method symbol from a candidate overload pointer set.
 * @param candidates Span of candidate function symbol pointers.
 * @param argumentTypes Deduced argument types for each argument expression.
 * @param symbolTable Symbol table for hierarchy and conversion lookups.
 * @param argIsLValue Optional flags indicating whether each argument expression is an L-value.
 * @return OverloadMatchResult containing the best candidate and match metrics.
 */
OverloadMatchResult ResolveBestOverload(std::span<const Symbol* const> candidates,
                                        const std::vector<std::string>& argumentTypes, const SymbolTable& symbolTable,
                                        const std::vector<bool>& argIsLValue = {});

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
bool HasSameParameterList(const Symbol& left, const Symbol& right);

/**
 * @brief Checks whether a type name string carries a const modifier.
 * @param typeName Type name string to inspect.
 * @return True if const modifier is present.
 */
bool HasConstModifier(std::string_view typeName);

bool IsPrimitiveWidening(const std::string& fromType, const std::string& toType);

/**
 * @brief Checks whether fromType to toType is a valid primitive narrowing or cross conversion.
 * @param fromType Source primitive type.
 * @param toType Destination primitive type.
 * @return True if narrowing conversion is valid.
 */
bool IsPrimitiveNarrowing(const std::string& fromType, const std::string& toType);
} // namespace angel_lsp::analysis
