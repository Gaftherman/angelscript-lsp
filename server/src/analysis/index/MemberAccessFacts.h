#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief AST-free representation of a member access expression (`object.member`).
 * @note Text views point into the source passed to BuildMemberAccessFacts, which must outlive this fact.
 */
struct MemberExpressionFact
{
    IndexedByteRange bytes;              ///< Byte span of the complete member expression.
    SourceRange range;                   ///< Line and column span of the complete member expression.
    IndexedByteRange objectBytes;        ///< Byte span of the object receiver operand.
    SourceRange objectRange;             ///< Line and column span of the object receiver.
    std::string_view objectText;         ///< Source spelling of the object receiver expression.
    IndexedByteRange memberBytes;        ///< Byte span of the member identifier token.
    SourceRange memberRange;             ///< Line and column span of the member identifier; anchors diagnostics.
    std::string_view memberName;         ///< Spelling of the member identifier.
    std::string_view enclosingClassName; ///< Innermost class enclosing this expression; empty if outside class or
                                         ///< inside lambda closure.
    bool insideMixin = false;            ///< True if the innermost enclosing class is a mixin class.
    bool isThisAccess = false;           ///< True if objectText is "this".
};

/**
 * @brief AST-free description of an identifier node that requires access permission checking.
 * @note Excludes declaration names, type references, comments, and member fields of member expressions.
 */
struct MemberAccessIdentifierFact
{
    IndexedByteRange bytes;      ///< Byte span of the identifier or this-expression node.
    SourceRange range;           ///< Line and column span used for diagnostics.
    std::string_view name;       ///< Cleaned identifier spelling (e.g. variable name, "this", "super", or scoped name).
    bool isThisOrSuper = false;  ///< True if name is "this" or "super".
    bool isInsideLambda = false; ///< True if enclosed by an intermediate lambda closure barrier.
    std::string_view
        enclosingClassName; ///< Innermost class enclosing this access; empty if outside class or inside lambda closure.
    bool insideMixin = false; ///< True if the enclosing class is a mixin class.
    std::string_view
        enclosingClassOfLambda; ///< Outer class enclosing the lambda, even crossing closure boundaries; empty if none.
};

/**
 * @brief AST-free inputs required to validate implicit member access on an unqualified identifier.
 */
struct ImplicitMemberAccessFact
{
    IndexedByteRange bytes; ///< Byte span of the identifier node; start byte keys scope lookups.
    SourceRange range;      ///< Line and column span used for diagnostics.
    std::string_view name;  ///< Identifier spelling.
    std::string_view
        enclosingClassName; ///< Innermost class enclosing this access; empty if outside class or inside lambda closure.
    bool insideMixin = false; ///< True if the enclosing class is a mixin class.
};

/**
 * @brief AST-free description of a lambda expression closure barrier and its outer class context.
 */
struct LambdaEnclosureFact
{
    IndexedByteRange bytes;              ///< Byte span of the lambda expression.
    SourceRange range;                   ///< Line and column span of the lambda expression.
    std::string_view enclosingClassName; ///< Outer class enclosing this lambda, or empty if outside any class.
    uint32_t identifierBegin = 0;        ///< First slice index into MemberAccessFacts::enclosedIdentifiers.
    uint32_t identifierCount = 0;        ///< Number of identifier spans inside this lambda enclosure.
};

/**
 * @brief Aggregated AST-free member access, identifier, and closure facts of one syntax revision.
 * @note All views point into the source passed to BuildMemberAccessFacts, which must outlive this object.
 */
struct MemberAccessFacts
{
    std::vector<MemberExpressionFact> memberExpressions; ///< Member expressions ordered by start byte.
    std::vector<MemberAccessIdentifierFact>
        identifiers; ///< Identifier nodes that need checking, ordered by start byte.
    std::vector<ImplicitMemberAccessFact>
        implicitAccesses;                              ///< Implicit member access candidates, ordered by start byte.
    std::vector<LambdaEnclosureFact> lambdaEnclosures; ///< Lambda closures ordered by start byte.
    std::vector<IndexedByteRange>
        enclosedIdentifiers; ///< Identifier spans enclosed in lambdas, sliced by LambdaEnclosureFact.

    /**
     * @brief Finds the member expression that spans exactly the given bytes.
     * @param[in] bytes Byte span of the member expression node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const MemberExpressionFact* FindMemberExpression(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the access identifier fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the identifier node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const MemberAccessIdentifierFact* FindIdentifier(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the implicit member access fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the identifier node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const ImplicitMemberAccessFact* FindImplicitAccess(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the lambda enclosure fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the lambda expression node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const LambdaEnclosureFact* FindLambdaEnclosure(IndexedByteRange bytes) const noexcept;
};

/**
 * @brief Extracts AST-free member access expressions, identifiers, and closure enclosures of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return Member access facts, ordered by source position.
 */
[[nodiscard]] MemberAccessFacts BuildMemberAccessFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
