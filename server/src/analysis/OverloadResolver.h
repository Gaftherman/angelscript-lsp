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
 * @brief Discrete match tiers matching official AngelScript compiler conversion categories (asCC_*).
 */
enum class MatchTier : uint8_t
{
    Exact = 0,              ///< Identity type match (or void to &out) (asCC_NO_CONV)
    ConstQualification = 1, ///< Const/reference qualification adjustment (asCC_CONST_CONV)
    SameSizeEnum = 2,       ///< Enum conversion of same size (asCC_ENUM_SAME_SIZE_CONV)
    DiffSizeEnum = 3,       ///< Enum conversion of different size (asCC_ENUM_DIFF_SIZE_CONV)
    PrimitiveWidening = 4,  ///< Primitive size-up promotion (asCC_PRIMITIVE_SIZE_UP_CONV)
    PrimitiveNarrowing = 5, ///< Primitive size-down truncation (asCC_PRIMITIVE_SIZE_DOWN_CONV)
    SignedToUnsigned = 6,   ///< Signed to unsigned integer (asCC_SIGNED_TO_UNSIGNED_CONV)
    UnsignedToSigned = 7,   ///< Unsigned to signed integer (asCC_UNSIGNED_TO_SIGNED_CONV)
    IntToFloat = 8,         ///< Integer to float conversion (asCC_INT_TO_FLOAT_CONV)
    FloatToInt = 9,         ///< Float to integer conversion (asCC_FLOAT_TO_INT_CONV)
    InheritanceRef = 10,    ///< Derived@ -> Base@ / Interface conversion (asCC_REF_CONV)
    ObjectToPrimitive = 12, ///< opConv to primitive (asCC_OBJ_TO_PRIMITIVE_CONV)
    ToObject = 14,          ///< Converting constructor or opImplConv to object (asCC_TO_OBJECT_CONV)
    VariableAny = 16,       ///< Wildcard / var type (?) (asCC_VARIABLE_CONV)
    UnknownTypes = 20,      ///< Fallback for unknown / unregistered host types
    RValueToOutParam = 30,  ///< R-value passed to an &out or mutable reference parameter
    Incompatible = 255      ///< No viable conversion exists
};

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
 * @brief Maps standard conversion subranks to canonical MatchTier.
 * @param[in] subRank Secondary subrank.
 * @param[in] isLossy True if lossy narrowing conversion.
 * @return Canonical MatchTier.
 */
[[nodiscard]] constexpr MatchTier StandardConvToTier(uint8_t subRank, bool isLossy) noexcept
{
    if (subRank == 220)
        return MatchTier::VariableAny;
    if (subRank == 100)
        return MatchTier::UnknownTypes;
    if (subRank == 200)
        return MatchTier::RValueToOutParam;
    if (subRank == 30)
        return MatchTier::IntToFloat;
    if (subRank == 40 || subRank == 50)
        return MatchTier::FloatToInt;
    if (subRank == 20 || subRank == 22)
        return MatchTier::SignedToUnsigned;
    if (isLossy)
        return MatchTier::PrimitiveNarrowing;
    return MatchTier::InheritanceRef;
}

/**
 * @brief Maps legacy ConversionRank and subRank to formal MatchTier.
 * @param[in] rank Legacy ConversionRank.
 * @param[in] subRank Secondary subrank.
 * @param[in] isConst True if const qualification adjustment.
 * @param[in] isLossy True if lossy narrowing conversion.
 * @return Canonical MatchTier.
 */
[[nodiscard]] constexpr MatchTier RankToTier(ConversionRank rank, uint8_t subRank, bool isConst, bool isLossy) noexcept
{
    switch (rank)
    {
    case ConversionRank::Exact:
        return isConst ? MatchTier::ConstQualification : MatchTier::Exact;
    case ConversionRank::Promotion:
        return MatchTier::PrimitiveWidening;
    case ConversionRank::StandardConv:
        return StandardConvToTier(subRank, isLossy);
    case ConversionRank::UserDefined:
        return MatchTier::ToObject;
    case ConversionRank::Incompatible:
    default:
        return MatchTier::Incompatible;
    }
}

/**
 * @brief Formal argument-to-parameter type conversion representation.
 */
struct ArgumentConversion
{
    MatchTier tier = MatchTier::Incompatible;
    ConversionRank rank = ConversionRank::Incompatible;
    uint8_t subRank = 0;             ///< Secondary ordering within rank
    uint8_t inheritanceDistance = 0; ///< Distance in inheritance hierarchy (0 = exact / same class)
    bool isConstAdjustment = false;  ///< Const/reference qualification adjustment
    int legacyScore = 999;           ///< Legacy scalar score for backwards compatibility
    bool isLossy = false;            ///< Conversion loses precision (narrowing / truncation)

    constexpr ArgumentConversion() noexcept = default;

    constexpr ArgumentConversion(MatchTier t, ConversionRank r, uint8_t sub = 0, uint8_t dist = 0, bool isConst = false,
                                 int score = 999, bool lossy = false) noexcept
        : tier(t), rank(r), subRank(sub), inheritanceDistance(dist), isConstAdjustment(isConst), legacyScore(score),
          isLossy(lossy)
    {
    }

    constexpr ArgumentConversion(ConversionRank r, uint8_t sub = 0, uint8_t dist = 0, bool isConst = false,
                                 int score = 999, bool lossy = false) noexcept
        : tier(RankToTier(r, sub, isConst, lossy)), rank(r), subRank(sub), inheritanceDistance(dist),
          isConstAdjustment(isConst), legacyScore(score), isLossy(lossy)
    {
    }

    /**
     * @brief Checks if conversion is viable for candidate invocation.
     * @return True if conversion rank is not Incompatible.
     */
    [[nodiscard]] constexpr bool IsViable() const noexcept
    {
        return tier != MatchTier::Incompatible && rank != ConversionRank::Incompatible;
    }

    /**
     * @brief Three-way lexicographical comparison between argument conversions.
     * @param[in] other Other conversion to compare against.
     * @return Strong ordering comparison result.
     */
    [[nodiscard]] auto operator<=>(const ArgumentConversion& other) const noexcept
    {
        if (auto cmp = tier <=> other.tier; cmp != 0)
        {
            return cmp;
        }
        if (auto cmp = rank <=> other.rank; cmp != 0)
        {
            return cmp;
        }
        if (auto cmp = isLossy <=> other.isLossy; cmp != 0)
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
    const Symbol* priorityCandidate = nullptr;       ///< Priority candidate when ambiguous or best match
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

    using ArityMap = ankerl::unordered_dense::map<size_t, std::vector<FunctionSymbol>>;
    using FunctionIndexMap =
        ankerl::unordered_dense::map<std::string, ArityMap, TransparentStringHash, std::equal_to<>>;

    /**
     * @brief Direct accessor to the internal function index map.
     * @return Const reference to internal map.
     */
    [[nodiscard]] const FunctionIndexMap& getFunctionIndex() const
    {
        return functionIndex_;
    }

  private:
    // Key: Function Identifier -> Map: Parameter Arity -> List of Candidates
    FunctionIndexMap functionIndex_;
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
bool HasSameParameterList(const Symbol& left, const Symbol& right, std::string_view arrayTypeName);
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

/**
 * @brief Checks whether a parameter accepts wildcard types (? or variable arguments).
 * @param param Parameter information to check.
 * @return True if parameter is wildcard.
 */
bool IsWildcardParameter(const ParameterInformation& param);
} // namespace angel_lsp::analysis
