#pragma once

#include "analysis/index/Symbol.h"
#include "analysis/types/TypeSanitization.h"

#include <ankerl/unordered_dense.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Builds the visited-edge key of a conversion from one type spelling to another.
 * @param[in] from Source type spelling.
 * @param[in] to Destination type spelling.
 * @return Owned key `from->to` that identifies the edge in a cycle guard set.
 */
[[nodiscard]] std::string MakeConversionEdgeKey(std::string_view from, std::string_view to);

/**
 * @brief The declared type of the single argument such an overload converts from.
 * @param[in] parameters Function parameters vector.
 * @return Canonicalized type name of the first argument.
 */
[[nodiscard]] std::string SingleArgumentType(const std::vector<ParameterInformation>& parameters);

namespace TypeConversionChecker
{
inline constexpr size_t MAX_CONVERSION_DEPTH = 32;

/**
 * @brief Simple type representation for conversion cycle checking.
 */
struct TypeInfo
{
    std::string name;

    bool operator==(const TypeInfo& other) const noexcept
    {
        return name == other.name;
    }
};

/**
 * @brief Evaluates whether user conversions can bridge source and target types.
 * @param[in] source Source type information.
 * @param[in] target Target type information.
 * @param[in] depth Current conversion depth.
 * @param[in,out] visitedEdges Active edge traversal set for cycle breaking.
 * @return True if a conversion path exists; false on cycle or depth limit.
 */
bool evaluateUserConversions(const TypeInfo& source, const TypeInfo& target, size_t depth,
                             ankerl::unordered_dense::set<std::string>& visitedEdges);

/**
 * @brief Checks implicit type conversion with recursion depth guard and cycle tracking.
 * @param[in] source Source type information.
 * @param[in] target Target type information.
 * @param[in] depth Current recursion depth (default 0).
 * @param[in,out] visitedEdges Active cycle tracking edge set.
 * @return True if implicitly convertible; false otherwise.
 */
bool canConvertImplicitly(const TypeInfo& source, const TypeInfo& target, size_t depth,
                          ankerl::unordered_dense::set<std::string>& visitedEdges);

/**
 * @brief Checks implicit type conversion with default parameters (depth = 0, empty visited set).
 * @param[in] source Source type information.
 * @param[in] target Target type information.
 * @param[in] depth Current recursion depth (default 0).
 * @return True if implicitly convertible; false otherwise.
 */
inline bool canConvertImplicitly(const TypeInfo& source, const TypeInfo& target, size_t depth = 0)
{
    ankerl::unordered_dense::set<std::string> visitedEdges;
    return canConvertImplicitly(source, target, depth, visitedEdges);
}
} // namespace TypeConversionChecker

/**
 * @brief Resolved expression type name, qualification modifiers, and value category.
 */
struct ExpressionType
{
    std::string baseName;   ///< Cleaned base type name, e.g. "Money" or "int".
    bool known = false;     ///< False means "give up" - never a reason to diagnose.
    bool isLiteral = false; ///< A literal's type is exact, so it can be judged strictly.
    bool isConst = false;   ///< True if expression has const-qualified type.
};

/**
 * @brief Declaration metadata for a resolved type, including type kind, scope, and modifiers.
 * @note Deliberately not the Symbol itself: this is looked up for practically every call
 *        expression in a document, and copying a Symbol (strings, parameter vectors, a
 */
struct TypeDeclarationInfo
{
    bool found = false;
    bool isClass = false;
    bool isTemplate = false;
};

/**
 * @brief Tracks active type conversions, ranking penalties, and constructor compatibility.
 */
struct ConversionState
{
    size_t depth = 0;
    ankerl::unordered_dense::set<std::string> visitedEdges;
};

/**
 * @brief RAII recursion guard preventing cyclic type conversion exploration.
 */
struct EdgeGuard
{
    ankerl::unordered_dense::set<std::string>& edges;
    std::string key;
    ~EdgeGuard()
    {
        edges.erase(key);
    }
};

/**
 * @brief Accessor resolution metadata for property get/set operations.
 */
struct PropertyAccessInfo
{
    bool isProperty = false;
    bool hasGet = false;
    bool hasSet = false;
    bool isIndexed = false;
    std::string propName;
    std::string receiverType;
};

/**
 * @brief Declared type name and qualification attributes extracted from variable or parameter declarations.
 *        server/src/analysis/SymbolTable.h.
 */
struct DeclaredType
{
    std::string baseName;
    TypeQualifiers qualifiers;
    bool usable = false; ///< False when the shape is out of scope (array, template, unknown).

    /**
     * @brief True for `array<T>` and `T[]`, where baseName is the *element* type.
     *        CleanBaseType answers the element type, which is what the initializer comparisons
     */
    bool isTemplateOrArray = false;
};

/**
 * @brief Tracks whether a type declares a zero-argument or default constructor.
 */
struct ZeroArgConstructorStatus
{
    bool hasZeroArg = false;
    bool isDeleted = false;
};

} // namespace angel_lsp::analysis
