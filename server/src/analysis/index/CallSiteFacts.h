#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <span>
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
    Member,     ///< Member access on an object or receiver (e.g. `obj.foo()` or `this.foo()`).
    Indirect    ///< Computed callee expression such as `callbacks[0](x)`, `make()(x)` or `(fn)(x)`.
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
 * @brief Consequence and alternative byte spans for a ternary call argument.
 */
struct CallArgumentTernaryFact
{
    IndexedByteRange consequenceBytes; ///< Byte span of the consequence branch expression.
    IndexedByteRange alternativeBytes; ///< Byte span of the alternative branch expression.
};

/**
 * @brief Compact lambda parameter metadata stored in CallSiteFacts::lambdaParams.
 */
struct CallLambdaParamFact
{
    IndexedByteRange typeBytes;                           ///< Byte span of the written parameter type node.
    ParameterModifier modifier = ParameterModifier::None; ///< Reference direction modifier (`in`, `out`, `inout`).
    bool hasWrittenType : 1 = false;                      ///< True when the parameter spells a type.
    bool isConst : 1 = false;                             ///< True when the written type carries `const`.
    bool isHandle : 1 = false;                            ///< True when the written type carries `@`.
    bool isReference : 1 = false;                         ///< True when `&` or a direction modifier is present.
};

/**
 * @brief Compact AST-free facts for a single argument in a call or constructor argument list.
 */
struct CallArgumentFact
{
    IndexedByteRange bytes;                               ///< Byte span of the argument expression.
    SourceRange range;                                    ///< Line and column span used for diagnostics.
    IndexedByteRange nameBytes;                           ///< Byte span of parameter name if named; empty otherwise.
    IndexedByteRange foldedBytes;                         ///< Byte span of unwrapped identifier if any.
    uint32_t detailIndex = 0;                             ///< Pool index into ternary, lambda, or member pools.
    uint16_t detailCount = 0;                             ///< Number of pool entries owned by this argument.
    CallArgumentKind kind = CallArgumentKind::Expression; ///< Syntactic classification of the argument.
    bool isNamed : 1 = false;                             ///< True if explicitly written as a named argument.
    bool isPositionalAfterNamed : 1 = false;              ///< True if positional argument follows a named argument.
    bool isThis : 1 = false;                              ///< True if argument expression is `this`.
    bool isNullLiteral : 1 = false;                       ///< True if argument expression is a null literal.
    bool isNumericLiteral : 1 = false;                    ///< True if argument expression is a numeric literal.
    bool hasLambdaParameterList : 1 = false;              ///< True when a lambda argument spells a parameter list.
    bool isThisMemberAccess : 1 = false;                  ///< True if argument is `this.<member>`.
};

/**
 * @brief AST-free representation of a call or construct-call expression site.
 */
struct CallSiteFact
{
    IndexedByteRange bytes;           ///< Byte span of the entire call expression.
    SourceRange range;                ///< Line and column span; start point selects lexical scope.
    IndexedByteRange calleeBytes;     ///< Byte span of the callee expression.
    SourceRange calleeRange;          ///< Line and column span of the callee expression.
    IndexedByteRange calleeNameBytes; ///< Byte span of the callee identifier token or member name.
    SourceRange calleeNameRange;      ///< Line and column span of callee identifier; anchors diagnostics.
    IndexedByteRange
        objectBytes; ///< Receiver object of a member call, or the plain root identifier of an indexed Indirect callee.
    IndexedByteRange enclosingClassNameBytes;    ///< Byte span of enclosing class or interface name; empty if outside.
    IndexedByteRange enclosingFunctionNameBytes; ///< Byte span of enclosing function or method name; empty if global.
    uint32_t argumentBegin = 0;                  ///< First slice index into CallSiteFacts::arguments.
    uint32_t argumentCount = 0;                  ///< Number of arguments in CallSiteFacts::arguments.
    CalleeKind calleeKind = CalleeKind::None;    ///< Classification of callee syntax (identifier, scoped, member).
    bool isConstructCall : 1 = false;            ///< True if construct_call_expression syntax (`Type(...)`).
    bool isMemberCall : 1 = false;               ///< True if callee is a member access (`obj.method()`).
    bool isThisCall : 1 = false;                 ///< True if object operand is `this` (`this.method()`).
    bool hasArgumentList : 1 = false;            ///< True if argument list syntax was present (`(...)`).
    bool hasNamedArguments : 1 = false;          ///< True if at least one named argument was supplied.
    bool hasPositionalAfterNamed : 1 = false;    ///< True if a positional argument follows a named argument.
    bool isDelegateConstruction : 1 = false;     ///< True if single argument is `this.<calleeName>`.
    bool insideClass : 1 = false;                ///< True if call is enclosed within a class or interface definition.
    bool insideMixin : 1 = false;                ///< True if enclosing class is a mixin class.
    bool isEnclosingMethodConst : 1 = false;     ///< True if call is enclosed within a const method declaration.
};

/**
 * @brief AST-free representation of a variable direct-initialization declarator (`Type var(args)`).
 */
struct DirectInitSiteFact
{
    IndexedByteRange bytes;             ///< Byte span of the variable declarator node.
    SourceRange range;                  ///< Line and column span; start point selects lexical scope.
    IndexedByteRange declaredTypeBytes; ///< Byte span of trimmed declared type in variable declaration.
    SourceRange argumentListRange;      ///< Line and column span of the constructor argument list.
    uint32_t argumentBegin = 0;         ///< First slice index into CallSiteFacts::arguments.
    uint32_t argumentCount = 0;         ///< Number of arguments in CallSiteFacts::arguments.
};

/**
 * @brief AST-free call, construct-call, and direct-initialization site facts of one revision.
 * @note Byte spans and TextOf() views refer to source passed to BuildCallSiteFacts, which must outlive this object.
 */
struct CallSiteFacts
{
    std::string_view source;                              ///< Document source text; must outlive this object.
    std::vector<CallSiteFact> callSites;                  ///< Call and construct-call sites ordered by start byte.
    std::vector<DirectInitSiteFact> directInits;          ///< Variable direct-initializations ordered by start byte.
    std::vector<CallArgumentFact> arguments;              ///< Flat arguments container sliced by begin/count indices.
    std::vector<CallArgumentTernaryFact> ternaryBranches; ///< Ternary argument branches indexed by CallArgumentFact.
    std::vector<CallLambdaParamFact> lambdaParams;        ///< Lambda argument parameters sliced by CallArgumentFact.
    std::vector<IndexedByteRange> thisMemberNames;        ///< Member spans for `this.<member>` call arguments.

    /**
     * @brief Slices a byte span out of an arbitrary source text buffer with bounds validation.
     * @param[in] sourceCode Document source buffer.
     * @param[in] bytes Half-open byte span within sourceCode.
     * @return View into sourceCode for bytes, or empty when out of bounds.
     */
    [[nodiscard]] static std::string_view SliceText(std::string_view sourceCode, IndexedByteRange bytes) noexcept
    {
        if (bytes.startByte >= sourceCode.size() || bytes.endByte > sourceCode.size() ||
            bytes.startByte >= bytes.endByte)
        {
            return {};
        }
        return sourceCode.substr(bytes.startByte, bytes.endByte - bytes.startByte);
    }

    /**
     * @brief Returns the source spelling covered by the given byte span.
     * @param[in] bytes Half-open byte span within source.
     * @return View into source, or empty when out of bounds.
     */
    [[nodiscard]] std::string_view TextOf(IndexedByteRange bytes) const noexcept
    {
        return SliceText(source, bytes);
    }

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

    /**
     * @brief Returns the argument slice owned by a call site.
     * @param[in] site Call site fact.
     * @return Span of argument facts, or empty when absent or out of range.
     */
    [[nodiscard]] std::span<const CallArgumentFact> ArgumentsOf(const CallSiteFact& site) const noexcept
    {
        if (site.argumentCount == 0 || site.argumentBegin >= arguments.size() ||
            site.argumentCount > arguments.size() - site.argumentBegin)
        {
            return {};
        }
        return std::span<const CallArgumentFact>(arguments.data() + site.argumentBegin, site.argumentCount);
    }

    /**
     * @brief Returns the argument slice owned by a direct initialization site.
     * @param[in] init Direct initialization site fact.
     * @return Span of argument facts, or empty when absent or out of range.
     */
    [[nodiscard]] std::span<const CallArgumentFact> ArgumentsOf(const DirectInitSiteFact& init) const noexcept
    {
        if (init.argumentCount == 0 || init.argumentBegin >= arguments.size() ||
            init.argumentCount > arguments.size() - init.argumentBegin)
        {
            return {};
        }
        return std::span<const CallArgumentFact>(arguments.data() + init.argumentBegin, init.argumentCount);
    }

    /**
     * @brief Returns the ternary consequence and alternative byte spans for a Ternary argument.
     * @param[in] arg Call argument fact of kind Ternary.
     * @return Branch span pair, or empty spans when absent.
     */
    [[nodiscard]] CallArgumentTernaryFact TernaryOf(const CallArgumentFact& arg) const noexcept
    {
        if (arg.kind != CallArgumentKind::Ternary || arg.detailCount == 0 || arg.detailIndex >= ternaryBranches.size())
        {
            return {};
        }
        return ternaryBranches[arg.detailIndex];
    }

    /**
     * @brief Returns the lambda parameter slice owned by a Lambda argument.
     * @param[in] arg Call argument fact of kind Lambda.
     * @return Span of compact lambda parameter facts, or empty when absent.
     */
    [[nodiscard]] std::span<const CallLambdaParamFact> LambdaParamsOf(const CallArgumentFact& arg) const noexcept
    {
        if (arg.kind != CallArgumentKind::Lambda || !arg.hasLambdaParameterList || arg.detailCount == 0)
        {
            return {};
        }
        const size_t begin = arg.detailIndex;
        const size_t count = arg.detailCount;
        if (begin >= lambdaParams.size() || count > lambdaParams.size() - begin)
        {
            return {};
        }
        return std::span<const CallLambdaParamFact>(lambdaParams.data() + begin, count);
    }

    /**
     * @brief Returns the member identifier spelling for a `this.<member>` argument.
     * @param[in] arg Call argument fact with isThisMemberAccess set.
     * @return Member name view into source, or empty when absent.
     */
    [[nodiscard]] std::string_view ThisMemberName(const CallArgumentFact& arg) const noexcept
    {
        if (arg.kind != CallArgumentKind::Member || !arg.isThisMemberAccess || arg.detailCount == 0 ||
            arg.detailIndex >= thisMemberNames.size())
        {
            return {};
        }
        return TextOf(thisMemberNames[arg.detailIndex]);
    }
};

/**
 * @brief Captures the spans, flags, and pooled details of a single call argument.
 * @param[in] exprNode Argument expression syntax node.
 * @param[in] nameNode Named argument identifier node, or null for positional arguments.
 * @param[in] index Precomputed node index of the document.
 * @param[in,out] facts Call site facts container owning source and detail pools.
 * @return Compact call argument fact.
 */
[[nodiscard]] CallArgumentFact ReadCallArgument(TSNode exprNode, TSNode nameNode, const NodeIndex& index,
                                                CallSiteFacts& facts);

/**
 * @brief Extracts all arguments from an argument list node into the call site facts sink.
 * @param[in] argListNode Argument list syntax node (`argument_list`).
 * @param[in] index Precomputed node index of the document.
 * @param[in,out] facts Destination call site facts container.
 */
void ExtractCallArguments(TSNode argListNode, const NodeIndex& index, CallSiteFacts& facts);

/**
 * @brief Captures all call, construct-call, and variable direct-initialization sites of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return Call site facts ordered by source position.
 */
[[nodiscard]] CallSiteFacts BuildCallSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
