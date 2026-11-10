#pragma once

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Classification of an assignment target expression.
 */
enum class ConstTargetKind : uint8_t
{
    Other,      ///< Unclassified or unsupported target expression.
    Identifier, ///< Plain identifier or scoped identifier (e.g. `foo` or `ns::foo`).
    Member,     ///< Member access on an arbitrary object (e.g. `obj.prop`).
    ThisMember, ///< Member access directly on 'this' (e.g. `this.field`).
    This,       ///< Direct 'this' expression (e.g. `this`).
    Index       ///< Index expression (e.g. `arr[0]`).
};

/**
 * @brief Unwrapped leaf operand details of a member expression chain.
 */
struct ConstChainLeafFact
{
    IndexedByteRange leafBytes; ///< Byte span of the leaf expression.
    std::string_view leafText;  ///< Source spelling of the leaf expression.
    bool leafIsThis = false;    ///< Leaf expression is `this`.
    bool inMemberChain = false; ///< Expression accessed members through a chain.
};

/**
 * @brief Syntactic details of an assignment target operand.
 */
struct ConstAssignmentTargetFact
{
    IndexedByteRange bytes;                        ///< Byte span of the target expression as written.
    SourceRange range;                             ///< Line and column span of the target for diagnostics.
    std::string_view text;                         ///< Source spelling of the target expression.
    ConstTargetKind kind = ConstTargetKind::Other; ///< Target kind classification.
    bool isHandle = false;                         ///< Target is wrapped in a unary `@` operator.
    IndexedByteRange actualTargetBytes;            ///< Byte span of the operand after unwrapping unary `@`.
    SourceRange actualTargetRange;                 ///< Line and column span after unwrapping unary `@`.
    std::string_view actualTargetText;             ///< Source spelling after unwrapping unary `@`.
    std::string_view memberName;                   ///< Member identifier if member or this-member; empty otherwise.
    IndexedByteRange receiverBytes;                ///< Receiver object byte span if member expression.
    std::string_view receiverText;                 ///< Receiver object spelling if member expression.
    IndexedByteRange leafBytes;                    ///< Leaf operand span at the root of a member chain.
    std::string_view leafText;                     ///< Leaf operand spelling at the root of a member chain.
    bool leafIsThis = false;                       ///< Leaf operand at root of member chain is `this`.
    bool inMemberChain = false;                    ///< Target expression involves one or more member accesses.
};

/**
 * @brief An assignment expression evaluated for const correctness.
 */
struct ConstAssignmentFact
{
    IndexedByteRange bytes;           ///< Byte span of the whole assignment expression.
    SourceRange range;                ///< Line and column span; start point selects lexical scope.
    ConstAssignmentTargetFact target; ///< Target operand on the left side of the assignment.
};

/**
 * @brief A method call site evaluated for required const qualifiers.
 */
struct ConstMethodCallFact
{
    IndexedByteRange bytes;       ///< Byte span of the call expression.
    SourceRange range;            ///< Line and column span; start point selects lexical scope.
    IndexedByteRange calleeBytes; ///< Byte span of the member expression callee.
    BinaryOperandFact object;     ///< Callee object operand.
    std::string_view methodName;  ///< Method identifier spelling.
    IndexedByteRange methodBytes; ///< Byte span of the method identifier.
    SourceRange methodRange;      ///< Line and column span of method identifier; anchors diagnostic.
    IndexedByteRange leafBytes;   ///< Leaf operand span at root of receiver member chain.
    std::string_view leafText;    ///< Leaf operand spelling at root of receiver member chain.
    bool leafIsThis = false;      ///< Leaf operand of receiver is `this`.
    bool inMemberChain = false;   ///< Receiver expression involves one or more member accesses.
};

/**
 * @brief Parameter declaration fact with its const qualifiers.
 */
struct ConstParameterFact
{
    IndexedByteRange bytes;     ///< Byte span of the parameter declaration.
    std::string_view name;      ///< Parameter identifier name.
    std::string_view typeText;  ///< Written parameter type spelling.
    bool isConst = false;       ///< Type has leading const qualifier (`const T`).
    bool isHandleConst = false; ///< Type has trailing handle const qualifier (`T@ const`).
};

/**
 * @brief Enclosing method or function scope information for const evaluation.
 */
struct EnclosingMethodFact
{
    IndexedByteRange bytes;      ///< Byte span of the function or method declaration.
    SourceRange range;           ///< Line and column span of the function or method.
    std::string_view className;  ///< Declaring class name; empty for non-member functions.
    std::string_view methodName; ///< Method or function identifier name.
    bool inClass = false;        ///< Declared within a class body.
    bool isConstMethod = false;  ///< Marked with const qualifier.
    uint32_t parameterBegin = 0; ///< First slice index into ConstSiteFacts::parameters.
    uint32_t parameterCount = 0; ///< Number of parameters declared on this method.
};

/**
 * @brief A callable scope that declares parameters (function, accessor, lambda, virtual property).
 */
struct ConstParameterScopeFact
{
    IndexedByteRange bytes;      ///< Byte span of the parameter owner declaration or expression.
    uint32_t parameterBegin = 0; ///< First slice index into ConstSiteFacts::parameters.
    uint32_t parameterCount = 0; ///< Number of parameters declared in this scope.
};

/**
 * @brief AST-free syntax facts for const correctness checks across one revision.
 * @note Views point into the source passed to BuildConstSiteFacts, which must outlive this object.
 */
struct ConstSiteFacts
{
    std::vector<ConstAssignmentFact> assignments;         ///< Ordered by start byte, then end byte descending.
    std::vector<ConstMethodCallFact> methodCalls;         ///< Ordered by start byte, then end byte descending.
    std::vector<EnclosingMethodFact> enclosingMethods;    ///< Ordered by start byte, then end byte descending.
    std::vector<ConstParameterScopeFact> parameterScopes; ///< Ordered by start byte, then end byte descending.
    std::vector<ConstParameterFact> parameters;           ///< Grouped by callable, source order.

    /**
     * @brief Finds the assignment fact spanning exactly the given bytes.
     * @param[in] bytes Byte span of the assignment expression.
     * @return Matching fact, or nullptr.
     */
    [[nodiscard]] inline const ConstAssignmentFact* FindAssignment(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(assignments, bytes);
    }

    /**
     * @brief Finds the method call fact spanning exactly the given bytes.
     * @param[in] bytes Byte span of the call expression.
     * @return Matching fact, or nullptr.
     */
    [[nodiscard]] inline const ConstMethodCallFact* FindMethodCall(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(methodCalls, bytes);
    }

    /**
     * @brief Finds the enclosing method fact spanning exactly the given bytes.
     * @param[in] bytes Byte span of the function or method declaration.
     * @return Matching fact, or nullptr.
     */
    [[nodiscard]] inline const EnclosingMethodFact* FindEnclosingMethod(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(enclosingMethods, bytes);
    }

    /**
     * @brief Finds the parameter scope fact spanning exactly the given bytes.
     * @param[in] bytes Byte span of the callable declaration or expression.
     * @return Matching fact, or nullptr.
     */
    [[nodiscard]] inline const ConstParameterScopeFact* FindParameterScope(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(parameterScopes, bytes);
    }

    /**
     * @brief Finds the innermost enclosing method or function spanning a byte offset.
     * @param[in] byteOffset Document byte position.
     * @return Innermost enclosing method, or nullptr.
     */
    [[nodiscard]] inline const EnclosingMethodFact* FindEnclosingMethodAt(uint32_t byteOffset) const noexcept
    {
        const EnclosingMethodFact* best = nullptr;
        for (const auto& method : enclosingMethods)
        {
            if (byteOffset >= method.bytes.startByte && byteOffset < method.bytes.endByte)
            {
                if (!best ||
                    (method.bytes.endByte - method.bytes.startByte) < (best->bytes.endByte - best->bytes.startByte))
                {
                    best = &method;
                }
            }
        }
        return best;
    }

    /**
     * @brief Finds a parameter by name in the innermost parameter scope spanning a byte offset.
     * @param[in] byteOffset Document byte position.
     * @param[in] name Parameter identifier name.
     * @return Matching parameter fact, or nullptr.
     */
    [[nodiscard]] inline const ConstParameterFact* FindParameter(uint32_t byteOffset,
                                                                 std::string_view name) const noexcept
    {
        const ConstParameterScopeFact* bestScope = nullptr;
        for (const auto& scope : parameterScopes)
        {
            if (byteOffset >= scope.bytes.startByte && byteOffset < scope.bytes.endByte)
            {
                if (!bestScope || (scope.bytes.endByte - scope.bytes.startByte) <
                                      (bestScope->bytes.endByte - bestScope->bytes.startByte))
                {
                    bestScope = &scope;
                }
            }
        }
        if (!bestScope)
        {
            return nullptr;
        }
        for (uint32_t i = 0; i < bestScope->parameterCount; ++i)
        {
            const auto& param = parameters[bestScope->parameterBegin + i];
            if (param.name == name)
            {
                return &param;
            }
        }
        return nullptr;
    }

    /**
     * @brief Returns a view of parameters declared on an enclosing method.
     * @param[in] method Method declaration fact.
     * @return Contiguous span of parameter facts.
     */
    [[nodiscard]] inline std::span<const ConstParameterFact>
    ParametersOf(const EnclosingMethodFact& method) const noexcept
    {
        return method.parameterBegin < parameters.size()
                   ? std::span<const ConstParameterFact>(parameters.data() + method.parameterBegin,
                                                         method.parameterCount)
                   : std::span<const ConstParameterFact>{};
    }

    /**
     * @brief Returns a view of parameters declared in a parameter scope.
     * @param[in] scope Owning callable parameter scope.
     * @return Contiguous span of parameter facts.
     */
    [[nodiscard]] inline std::span<const ConstParameterFact>
    ParametersOf(const ConstParameterScopeFact& scope) const noexcept
    {
        return scope.parameterBegin < parameters.size()
                   ? std::span<const ConstParameterFact>(parameters.data() + scope.parameterBegin, scope.parameterCount)
                   : std::span<const ConstParameterFact>{};
    }
};

/**
 * @brief Captures all syntax facts required for const correctness analysis of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text buffer; must outlive the returned facts.
 * @return Const site facts ordered for exact-span and positional lookups.
 */
[[nodiscard]] ConstSiteFacts BuildConstSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
