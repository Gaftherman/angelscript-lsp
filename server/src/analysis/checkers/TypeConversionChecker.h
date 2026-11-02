#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

#include <ankerl/unordered_dense.h>
#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks that every conversion written in a document has a declaration backing it.
 *        AngelScript reaches a target type through four routes, and each one is a declaration this
 */

struct TypeConversionCheckRequest
{
    /**
     * @brief Root node of the document's syntax tree.
     */
    TSNode root;

    /**
     * @brief Document source text the tree was parsed from.
     */
    std::string_view sourceCode;

    /**
     * @brief Root of the document's lexical scope tree, or nullptr when none was collected.
     */
    const Scope* scopeRoot = nullptr;

    /**
     * @brief The same tree as scopeRoot, non-null only while the caller exclusively owns it.
     *        `auto` inference writes the deduced type back into the LocalDefinition so that everything
     */
    Scope* mutableScopeRoot = nullptr;

    /**
     * @brief Optional pre-indexed node index for fast single-pass querying.
     */
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Walks the document and reports conversions with no declaration to back them.
 * @param request Tree, source text and scope tree of the document under analysis.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void CheckTypeConversions(const TypeConversionCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Whether a value of `fromType` can reach `toType` implicitly.
 *        The same judgement CheckTypeConversions makes internally, exposed for the one caller that has
 * @param fromType Source type, as spelled - decorations are stripped internally.
 * @param toType Target type, likewise.
 * @param ctx Diagnostic sink; used only for the SymbolTable and config it carries.
 */
bool CanConvertImplicitly(const std::string& fromType, const std::string& toType, const DiagnosticContext& ctx);

/**
 * @brief Evaluates whether a type satisfies boolean truthiness in control flow conditions.
 *        In AngelScript, conditions (if, while, for, do-while, ternary) accept:
 * @param typeName Type name to evaluate.
 * @param table SymbolTable for looking up class declarations and methods.
 * @return True if the type can evaluate as a condition; false otherwise.
 */
bool IsTruthyCondition(const std::string& typeName, const SymbolTable& table);

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

inline bool IsTruthyCondition(const std::string& typeName, const SymbolTable& table)
{
    return ::angel_lsp::analysis::IsTruthyCondition(typeName, table);
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

/**
 * @brief Left and right operand types and value categories in an assignment expression.
 */
struct AssignmentOperands
{
    TSNode left;
    TSNode right;
};

/**
 * @brief Analysis engine for recursive expression value evaluation and type convertibility queries.
 */
struct TypeConversionEvaluator
{
    /**
     * @brief Determines whether a value of source type is convertible to target type.
     * @param[in] from Source type name string.
     * @param[in] to Target destination type name string.
     * @param[in] ctx Diagnostic context containing symbol table and active type configuration.
     * @param[in,out] state Active conversion exploration state and edge guards.
     * @return True if convertible; false otherwise.
     */
    static bool IsConvertible(const std::string& from, const std::string& to, const DiagnosticContext& ctx,
                              ConversionState& state);

    /**
     * @brief Resolves the expression type and constness denotation for an AST value node.
     * @param[in] node AST expression node to resolve.
     * @param[in] scope Lexical scope.
     * @param[in] ctx Diagnostic sink and evaluation context.
     * @return Resolved ExpressionType.
     */
    static ExpressionType ResolveValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx);
};
} // namespace angel_lsp::analysis
