#pragma once

#include "analysis/index/AssignmentSiteFacts.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/ValueExpressionFacts.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Classification of function or method invocation callee syntax.
 */
enum class CalleeKind : uint8_t
{
    None,       ///< Missing, unparsed, or unsupported callee expression.
    Identifier, ///< Unqualified identifier or keyword function (e.g. `foo()`).
    Scoped,     ///< Scoped or namespace-qualified identifier (e.g. `ns::foo()` or `Math::sin()`).
    Member      ///< Member access on an object or receiver (e.g. `obj.foo()` or `this.foo()`).
};

/**
 * @brief Syntactic classification of a call argument expression.
 */
enum class CallArgumentKind : uint8_t
{
    Expression,      ///< General expression.
    Literal,         ///< Literal value (numeric, string, boolean, or null).
    Identifier,      ///< Unqualified or scoped identifier.
    Member,          ///< Member access expression.
    Lambda,          ///< Lambda expression (`function(...) { ... }`).
    InitializerList, ///< Initializer list expression (`{ ... }`).
    Ternary          ///< Ternary conditional expression (`cond ? a : b`).
};

/**
 * @brief AST-free facts for a single argument in a call or constructor argument list.
 */
struct CallArgumentFact
{
    IndexedByteRange bytes;              ///< Byte span of the argument expression.
    SourceRange range;                   ///< Line and column span used for diagnostics.
    std::string_view text;               ///< Raw source spelling of the argument expression.
    std::string_view name;               ///< Parameter name if named argument; empty for positional.
    SourceRange nameRange;               ///< Line and column span of the parameter name identifier.
    bool isNamed = false;                ///< True if explicitly written as a named argument.
    bool isPositionalAfterNamed = false; ///< True if this positional argument follows a named argument.
    CallArgumentKind kind = CallArgumentKind::Expression; ///< Syntactic classification of the argument.
    BinaryOperandFact operand;            ///< Binary operand fact with constant-folding and unary syntax flags.
    ValueExpressionFact value;            ///< Value expression fact; input of ResolveValueType.
    FuncdefValueFact funcdefValue;        ///< Lambda parameter list and funcdef values; filled for lambdas.
    bool isThisMemberAccess = false;      ///< True if argument is a member access on `this` (`this.name`).
    std::string_view thisMemberName;      ///< Member identifier if isThisMemberAccess is true.
    bool isTernary = false;               ///< True if argument is a ternary conditional expression.
    BinaryOperandFact ternaryConsequence; ///< Consequence branch operand if ternary expression.
    BinaryOperandFact ternaryAlternative; ///< Alternative branch operand if ternary expression.
};

/**
 * @brief AST-free representation of a call or construct-call expression site.
 */
struct CallSiteFact
{
    IndexedByteRange bytes;                   ///< Byte span of the entire call expression.
    SourceRange range;                        ///< Line and column span; start point selects lexical scope.
    bool isConstructCall = false;             ///< True if construct_call_expression syntax (`Type(...)`).
    CalleeKind calleeKind = CalleeKind::None; ///< Classification of callee syntax (identifier, scoped, member).
    IndexedByteRange calleeBytes;             ///< Byte span of the callee expression.
    SourceRange calleeRange;                  ///< Line and column span of the callee expression.
    std::string_view calleeText;              ///< Raw source spelling of the callee expression.
    IndexedByteRange calleeNameBytes;         ///< Byte span of the callee identifier token or member name.
    SourceRange calleeNameRange;              ///< Line and column span of callee identifier; anchors diagnostics.
    std::string_view calleeName;              ///< Unqualified identifier or member name (e.g. `foo`).
    std::string_view scopeQualifier;          ///< Scope qualifier prefix for scoped callee (e.g. `ns` in `ns::foo`).
    bool isMemberCall = false;                ///< True if callee is a member access (`obj.method()`).
    BinaryOperandFact objectOperand;          ///< Object receiver operand fact for member calls.
    ValueExpressionFact objectValue;          ///< Object receiver read as a value expression.
    bool isThisCall = false;                  ///< True if object operand is `this` (`this.method()`).
    bool hasTemplateArguments = false;        ///< True if explicit template argument list is present.
    IndexedByteRange templateArgsBytes;       ///< Byte span of the template argument list.
    std::string_view templateArgsText;        ///< Template argument list spelling (e.g. `<int, string>`).
    bool hasArgumentList = false;             ///< True if argument list syntax was present (`(...)`).
    IndexedByteRange argumentListBytes;       ///< Byte span of the argument list.
    SourceRange argumentListRange;            ///< Line and column span of the argument list.
    uint32_t argumentBegin = 0;               ///< First slice index into CallSiteFacts::arguments.
    uint32_t argumentCount = 0;               ///< Number of arguments in CallSiteFacts::arguments.
    bool hasNamedArguments = false;           ///< True if at least one named argument was supplied.
    bool hasPositionalAfterNamed = false;     ///< True if a positional argument follows a named argument.
    bool isDelegateConstruction = false;      ///< True if single argument is `this.<calleeName>`.
    std::string_view enclosingClassName;      ///< Innermost enclosing class or interface name; empty if outside.
    bool insideClass = false;                 ///< True if call is enclosed within a class or interface definition.
    bool insideMixin = false;                 ///< True if enclosing class is a mixin class.
    bool isInsideLambda = false;              ///< True if call is enclosed within an intermediate lambda closure.
    std::string_view enclosingFunctionName;   ///< Enclosing function or method declaration name; empty if global.
    bool isEnclosingMethodConst = false;      ///< True if call is enclosed within a const method declaration.
};

/**
 * @brief AST-free representation of a variable direct-initialization declarator (`Type var(args)`).
 */
struct DirectInitSiteFact
{
    IndexedByteRange bytes;               ///< Byte span of the variable declarator node.
    SourceRange range;                    ///< Line and column span; start point selects lexical scope.
    std::string_view declaredType;        ///< Raw declared type spelling from variable declaration.
    std::string_view baseTypeName;        ///< Base type name without template arguments (e.g. `Foo`).
    std::string_view templateArgsText;    ///< Template arguments spelling if parameterized (e.g. `<int>`).
    IndexedByteRange declaratorNameBytes; ///< Byte span of the variable name identifier.
    SourceRange declaratorNameRange;      ///< Line and column span of the variable name identifier.
    std::string_view declaratorName;      ///< Variable name identifier spelling.
    IndexedByteRange argumentListBytes;   ///< Byte span of the constructor argument list.
    SourceRange argumentListRange;        ///< Line and column span of the constructor argument list.
    uint32_t argumentBegin = 0;           ///< First slice index into CallSiteFacts::arguments.
    uint32_t argumentCount = 0;           ///< Number of arguments in CallSiteFacts::arguments.
};

/**
 * @brief AST-free call, construct-call, and direct-initialization site facts of one revision.
 * @note All string_view fields point into the source passed to BuildCallSiteFacts, which must outlive this object.
 */
struct CallSiteFacts
{
    std::vector<CallSiteFact> callSites;         ///< Call and construct-call sites ordered by start byte.
    std::vector<DirectInitSiteFact> directInits; ///< Variable direct-initializations ordered by start byte.
    std::vector<CallArgumentFact> arguments;     ///< Flat arguments container sliced by begin/count indices.

    /**
     * @brief Finds the call or construct-call site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the call expression node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const CallSiteFact* FindCallSite(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the variable direct-initialization site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the variable declarator node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const DirectInitSiteFact* FindDirectInit(IndexedByteRange bytes) const noexcept;
};

/**
 * @brief Captures the spans, spelling, and syntactic facts of a single call argument.
 * @param[in] exprNode Argument expression syntax node.
 * @param[in] nameNode Named argument identifier node, or null for positional arguments.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; the fact's views point into it.
 * @return Call argument fact.
 */
[[nodiscard]] CallArgumentFact ReadCallArgument(TSNode exprNode, TSNode nameNode, const NodeIndex& index,
                                                std::string_view source);

/**
 * @brief Extracts all arguments from an argument list node into the call site facts sink.
 * @param[in] argListNode Argument list syntax node (`argument_list`).
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; the facts' views point into it.
 * @param[in,out] facts Destination call site facts container.
 */
void ExtractCallArguments(TSNode argListNode, const NodeIndex& index, std::string_view source, CallSiteFacts& facts);

/**
 * @brief Captures all call, construct-call, and variable direct-initialization sites of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; the facts' views point into it.
 * @return Call site facts ordered by source position.
 */
[[nodiscard]] CallSiteFacts BuildCallSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
