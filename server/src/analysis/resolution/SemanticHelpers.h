#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "config/EngineRuleConfig.h"
#include "parser/ASTUtils.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <ankerl/unordered_dense.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
namespace node_types
{
constexpr std::string_view StringLiteral = "string_literal";
constexpr std::string_view LambdaExpression = "lambda_expression";
constexpr std::string_view BooleanLiteral = "boolean_literal";
constexpr std::string_view NumberLiteral = "number_literal";
constexpr std::string_view ImportDeclaration = "import_declaration";
constexpr std::string_view NullLiteral = "null_literal";
constexpr std::string_view CallExpression = "call_expression";
} // namespace node_types

/**
 * @brief Classification categories for initializer list item expressions.
 */
enum class InitializerItemKind
{
    NumericOrExpression,
    StringLiteral,
    BooleanLiteral,
    NullLiteral,
    NestedInitializer
};

/**
 * @brief Checks if a symbol with the specified type exists in the symbol table.
 * @param[in] table Symbol table to query.
 * @param[in] name Symbol identifier or qualified name.
 * @param[in] type Target symbol type.
 * @return True if a matching symbol exists; false otherwise.
 */
[[nodiscard]] inline bool HasSymbolOfType(const SymbolTable& table, std::string_view name, SymbolType type) noexcept
{
    const auto syms = table.FindSymbolsPtr(name);
    return syms && std::any_of(syms->begin(), syms->end(), [type](const Symbol& s) { return s.type == type; });
}

/**
 * @brief Checks if a symbol's declaring container has the specified symbol type.
 * @param[in] sym Symbol whose container to inspect.
 * @param[in] table Symbol table to query.
 * @param[in] type Target container symbol type.
 * @return True if the container exists and matches the specified type; false otherwise.
 */
[[nodiscard]] inline bool HasContainerOfType(const Symbol& sym, const SymbolTable& table, SymbolType type) noexcept
{
    if (sym.containerName.empty())
    {
        return false;
    }
    const auto container = table.FindSymbolsPtr(sym.containerName);
    return container && std::any_of(container->begin(), container->end(),
                                    [type](const Symbol& owner) { return owner.type == type; });
}

/**
 * @brief Checks whether a definition's declaring scope is inside any function/method/lambda body.
 * @param[in] defScope Definition declaring scope.
 * @return True if enclosed within a function scope.
 */
[[nodiscard]] inline bool IsDeclaredInFunctionScope(const Scope* defScope)
{
    size_t depth = 0;
    ankerl::unordered_dense::set<const Scope*> visited;
    for (const Scope* cur = defScope; cur != nullptr; cur = cur->parent)
    {
        if (++depth > kMaxScopeDepth || !visited.insert(cur).second)
        {
            break;
        }
        if (cur->isFunctionScope)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Extracts a non-allocating string_view corresponding to an AST node's byte range.
 * @param[in] node Tree-Sitter AST node.
 * @param[in] sourceCode Document source text buffer.
 * @return Non-owning string_view of the node's slice, or empty view if node is null.
 */
[[nodiscard]] std::string_view GetNodeTextView(TSNode node, std::string_view sourceCode) noexcept;

[[nodiscard]] std::string GetTrimmedNodeText(TSNode node, std::string_view sourceCode);

/**
 * @brief Normalizes a type name by stripping const, references, and whitespace.
 * @param typeName The raw type name string.
 * @return Cleaned type name.
 */
std::string CleanExpressionType(std::string_view typeName);

/**
 * @brief Checks whether the given name is a reserved AngelScript keyword.
 * @param name Symbol name to check.
 * @return True if name is a reserved keyword.
 */
bool IsReservedKeyword(const std::string& name);

/**
 * @brief Checks whether the given word is any AngelScript keyword (reserved or contextual).
 * @param word Word to check.
 * @return True if word is a keyword.
 */
[[nodiscard]] bool IsKeyword(std::string_view word) noexcept;

/**
 * @brief Checks if a tree-sitter operator symbol is a comparison or logical boolean binary operator.
 * @param[in] sym Grammar symbol of the operator token.
 * @param[in] gs Precompiled grammar symbols table.
 * @return True if operator produces a boolean result.
 */
[[nodiscard]] bool IsBooleanBinaryOpSym(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept;

/**
 * @brief Checks if an argument count falls within min and max argument bounds for a callable reference.
 * @param[in] argCount Observed argument count.
 * @param[in] minArgs Minimum required arguments.
 * @param[in] maxArgs Maximum permitted arguments.
 * @return True if argCount is within [minArgs, maxArgs].
 */
[[nodiscard]] constexpr bool MatchesArgumentCount(size_t argCount, size_t minArgs, size_t maxArgs) noexcept
{
    return argCount >= minArgs && argCount <= maxArgs;
}

/**
 * @brief Checks if a call reference satisfies target/criteria argument count bounds.
 * @tparam RefLike Reference object possessing isCall (bool) and argumentCount (size_t/int).
 * @tparam TargetLike Target object possessing isFunction (bool), minArgs (int/size_t), and maxArgs (int/size_t).
 * @param[in] ref Local reference to check.
 * @param[in] target Target or criteria constraints.
 * @return True if argument count is within bounds, or reference is not a function call.
 */
template <typename RefLike, typename TargetLike>
    requires requires(const RefLike& r, const TargetLike& t) {
        { r.isCall } -> std::convertible_to<bool>;
        { r.argumentCount } -> std::convertible_to<size_t>;
        { t.isFunction } -> std::convertible_to<bool>;
        { t.minArgs } -> std::convertible_to<size_t>;
        { t.maxArgs } -> std::convertible_to<size_t>;
    }
[[nodiscard]] constexpr bool MatchesCallArguments(const RefLike& ref, const TargetLike& target) noexcept
{
    if (target.isFunction && ref.isCall)
    {
        return MatchesArgumentCount(ref.argumentCount, target.minArgs, target.maxArgs);
    }
    return true;
}

/**
 * @brief Immutable core AngelScript primitive types.
 */
[[nodiscard]] constexpr bool IsCorePrimitive(std::string_view typeName) noexcept
{
    // `auto` is here and is not a primitive - it is not a type at all, but a stand-in for
    // whatever the initializer produces. Callers that care about the difference test for it by
    // name; see the handle-on-primitive rule in SemanticAnalyzer, which had to.
    return parser::primitives::IsPrimitive(typeName) || typeName == "auto";
}

/**
 * @brief True for AngelScript's integer primitives, signed or unsigned.
 *        `int32`/`uint32` are the explicit spellings of `int`/`uint` and are listed alongside them:
 */
[[nodiscard]] constexpr bool IsIntegerPrimitive(std::string_view typeName) noexcept
{
    return parser::primitives::IsInteger(typeName);
}

/**
 * @brief Returns the last unqualified identifier segment of a scoped name.
 * @param[in] name Qualified or unqualified identifier.
 * @return View of the last segment without allocations.
 */
[[nodiscard]] std::string_view LastScopeSegment(std::string_view name) noexcept;

/**
 * @brief Returns the parent scope qualifier of a scoped name (everything preceding the last `::`).
 * @param[in] name Qualified name.
 * @return Parent scope view, or empty view if name contains no `::`.
 */
[[nodiscard]] std::string_view ParentScope(std::string_view name) noexcept;

/**
 * @brief Checks if two type or class names match directly or by short scope segment.
 * @param[in] a First type name.
 * @param[in] b Second type name.
 * @return True if names match directly or by unqualified segment.
 */
[[nodiscard]] inline bool MatchesDeclOrScope(std::string_view a, std::string_view b) noexcept
{
    if (a == b)
    {
        return true;
    }
    auto segA = LastScopeSegment(a);
    auto segB = LastScopeSegment(b);
    return a == segB || segA == b || segA == segB;
}

/**
 * @brief Checks if container is identical to or nested inside namespace ns without heap allocation.
 */
[[nodiscard]] constexpr bool IsSameOrNestedNamespace(std::string_view container, std::string_view ns) noexcept
{
    if (ns.empty())
    {
        return false;
    }
    if (container == ns)
    {
        return true;
    }
    return container.size() > ns.size() + 2 && container.starts_with(ns) && container[ns.size()] == ':' &&
           container[ns.size() + 1] == ':';
}

/**
 * @brief Checks if full matches suffix or ends with "::" + suffix without heap allocation.
 */
[[nodiscard]] constexpr bool EndsWithScopedSuffix(std::string_view full, std::string_view suffix) noexcept
{
    if (suffix.empty())
    {
        return false;
    }
    if (full == suffix)
    {
        return true;
    }
    return utils::EndsWithScopedMember(full, suffix);
}

/**
 * @brief Checks if a scoped identifier exactly equals prefix::suffix without heap string allocation.
 * @param[in] scoped The full scoped identifier to test.
 * @param[in] prefix The expected scope prefix.
 * @param[in] suffix The expected identifier suffix.
 * @return True if scoped exactly matches prefix + "::" + suffix.
 */
[[nodiscard]] constexpr bool MatchesScopedPair(std::string_view scoped, std::string_view prefix,
                                               std::string_view suffix) noexcept
{
    return scoped.size() == prefix.size() + utils::k_scopeSeparator.size() + suffix.size() &&
           scoped.starts_with(prefix) && scoped.ends_with(suffix) &&
           scoped.substr(prefix.size(), utils::k_scopeSeparator.size()) == utils::k_scopeSeparator;
}

/**
 * @brief Checks if a qualified symbol target ends with suffix preceded by a scope delimiter "::".
 * @param[in] target Enclosing qualified string.
 * @param[in] suffix Candidate trailing qualifier segment.
 * @return True if target ends with "::" + suffix.
 */
[[nodiscard]] constexpr bool SuffixMatchesScoped(std::string_view target, std::string_view suffix) noexcept
{
    return utils::EndsWithScopedMember(target, suffix);
}

/**
 * @brief Checks if an identifier qualifier matches an enum's declaring type or its enclosing namespace.
 * @param[in] qualifier Qualifier prefix extracted from scoped reference (e.g. "INS2BASE" or "INS2_SELECTFIRE_OPTIONS").
 * @param[in] declaringEnum Declaring enum qualified name (e.g. "INS2BASE::INS2_SELECTFIRE_OPTIONS").
 * @return True if qualifier matches the enum name, full enum path, or enclosing namespace.
 */
[[nodiscard]] constexpr bool EnumQualifierMatches(std::string_view qualifier, std::string_view declaringEnum) noexcept
{
    while (qualifier.starts_with(':') || qualifier.starts_with(' ') || qualifier.starts_with('\t'))
    {
        qualifier.remove_prefix(1);
    }
    if (qualifier.empty() || declaringEnum.empty())
    {
        return false;
    }
    if (qualifier == declaringEnum || SuffixMatchesScoped(declaringEnum, qualifier))
    {
        return true;
    }
    const std::string_view nsPrefix = utils::ParentScopeView(declaringEnum);
    if (!nsPrefix.empty())
    {
        return qualifier == nsPrefix || SuffixMatchesScoped(nsPrefix, qualifier);
    }
    return false;
}

/**
 * @brief Strips leading '@' or 'this.' from candidate identifier strings.
 */
[[nodiscard]] std::string_view CleanCandidateIdentifier(std::string_view text) noexcept;

/**
 * @brief Validates if string is a valid identifier or qualified scoped identifier (e.g. NS::Class).
 */
[[nodiscard]] bool IsValidIdentifierOrQualified(std::string_view text) noexcept;

[[nodiscard]] constexpr bool IsFloatingPointPrimitive(std::string_view typeName) noexcept
{
    return parser::primitives::IsFloatingPoint(typeName);
}

/**
 * @brief True for any primitive that carries a number - integer or floating point.
 *        Excludes `bool`, which converts to and from the numeric types but is not one of them; callers
 */
[[nodiscard]] constexpr bool IsNumericPrimitive(std::string_view typeName) noexcept
{
    return IsIntegerPrimitive(typeName) || IsFloatingPointPrimitive(typeName);
}

/**
 * @brief Configurable type resolution against host TypeConfig.
 */
[[nodiscard]] inline bool IsPredefinedOrRegisteredType(std::string_view typeName,
                                                       const config::TypeConfig& typeConfig) noexcept
{
    if (IsCorePrimitive(typeName))
    {
        return true;
    }
    if (typeName == typeConfig.stringTypeName || typeName == typeConfig.arrayTypeName)
    {
        return true;
    }
    return typeConfig.registeredSymbols.contains(std::string(typeName));
}

/**
 * @brief Checks whether the given name is a primitive AngelScript type name.
 * @param name Symbol or type name to check.
 * @return True if name is a primitive type name.
 */
bool IsPrimitiveTypeName(std::string_view name);

/**
 * @brief Semantically classifies the token text of an initializer item.
 * @param item Token or expression text of the initializer item.
 * @return InitializerItemKind classification.
 */
InitializerItemKind ClassifyInitializerItem(std::string_view item);

/**
 * @brief True when the symbol was declared in a predefined stub.
 *        Stubs describe a host application's API that the engine already accepted, so the rules about
 * @param sym Symbol whose declaring file is in question.
 * @param ctx DiagnosticContext carrying the configured stub suffix.
 */
bool IsFromPredefinedStub(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Checks whether a function symbol is a destructor declaration given document source text.
 * @param[in] sym Function symbol to classify.
 * @param[in] source Document source text.
 * @return True if the symbol is a destructor declaration.
 */
bool IsDestructorDeclaration(const Symbol& sym, std::string_view source);

bool IsDestructorDeclaration(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Checks whether a constructor function matches AngelScript's list behavior signature.
 *        Conforming to `asBEHAVE_LIST_CONSTRUCT` / `asBEHAVE_LIST_FACTORY`:
 * @param[in] fn Function signature to inspect.
 * @return True if function signature conforms to a list constructor.
 */
bool IsListConstructorSignature(const FunctionSignature& fn);

/**
 * @brief Checks whether a symbol represents a list constructor.
 * @param[in] sym Symbol to inspect.
 * @return True if symbol is a function conforming to a list constructor.
 */
bool IsListConstructor(const Symbol& sym);

/**
 * @brief Names the function attribute a declaration carries, or empty when it carries none.
 *        The attributes are five independent flags on SymbolModifiers, but every rule that rejects
 * @param modifiers Modifier set to inspect.
 * @return Attribute name in the order the grammar lists them, or an empty view.
 */
std::string_view FirstAttributeName(const SymbolModifiers& modifiers);

/**
 * @brief True when a name resolves to a function and to nothing that could be a type.
 *        The shape behind `void Foo(int) {}` followed by `Foo@ h`: the compiler answers "Identifier
 */
bool NamesAFunctionNotAType(std::string_view name, const class SymbolTable& table);

/**
 * @brief Checks whether an AST node is a bare data type identifier used in a value expression context.
 * @param node AST node to examine.
 * @param scope Local scope at the node, or nullptr.
 * @param symbolTable Symbol table for type and value lookup.
 * @param sourceCode Source text of the document.
 * @return Resolved data type name if the node is a bare data type without value semantics, or std::nullopt.
 */
std::optional<std::string> IsBareDataType(TSNode node, const struct Scope* scope, const class SymbolTable& symbolTable,
                                          std::string_view sourceCode);

/**
 * @brief Checks if an expression denotes a bare function or method reference.
 * @param[in] node AST expression node.
 * @param[in] scope Lexical scope at the node, or nullptr.
 * @param[in] symbolTable Symbol table for symbol lookups.
 * @param[in] sourceCode Source text of the document.
 * @return True if the expression denotes a bare function or method without invocation.
 */
bool IsFunctionReference(TSNode node, const struct Scope* scope, const class SymbolTable& symbolTable,
                         std::string_view sourceCode);

/**
 * @brief Whether `node` is the callee of a legal `super(...)` base-constructor call.
 *        `super` is not a general reference to the base class. The compiler answers
 * @param node Any node whose text is the identifier `super` - the callee of a call expression.
 * @param sourceCode Document source text.
 * @return True when the enclosing function is a constructor of a class with a base list.
 */
bool IsBaseConstructorCall(TSNode node, std::string_view sourceCode);

/**
 * @brief Pair of source and destination type names for conversion evaluation.
 */
struct ConversionTypePair
{
    std::string from;
    std::string to;
};

/**
 * @brief Checks whether a text expression represents a simple literal constant value.
 * @param[in] expr Expression text as written in source code.
 * @return True if expression is a number, boolean, or string literal.
 */
bool IsSimpleLiteralExpression(std::string_view expr);

/**
 * @brief Extracts the preceding scope qualifier for a datatype node, if any.
 * @param[in] datatypeNode AST datatype node.
 * @param[in] sourceCode Document source text.
 * @return Scope prefix string ending in "::", or empty if unqualified.
 */
std::string ExtractDatatypeScopePrefix(TSNode datatypeNode, std::string_view sourceCode);

/**
 * @brief Result of binary operator overload resolution containing candidate symbol and parameter conversions.
 */
struct BinaryOverloadResult
{
    bool hasMatch = false;
    std::vector<ArgumentConversion> bestConversions;
    std::string returnType;
};

} // namespace angel_lsp::analysis
