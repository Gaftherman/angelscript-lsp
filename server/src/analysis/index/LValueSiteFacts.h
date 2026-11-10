#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Syntactic classification of an expression evaluated as an l-value candidate.
 */
enum class LValueCandidateKind : uint8_t
{
    None,             ///< Missing operand, null node, or unrecognized expression.
    Identifier,       ///< Identifier, scoped identifier, or tokFunction symbol.
    Member,           ///< Member access expression (object.member).
    Index,            ///< Index expression (e.g. array[i]), always assignable.
    UnaryDereference, ///< Unary handle dereference (@operand).
    Ternary,          ///< Ternary conditional expression (condition ? consequence : alternative).
    Call,             ///< Call expression target (handled via call l-value return reference checks).
    Other             ///< Non-assignable expression (literal, binary operator, cast, etc.).
};

/**
 * @brief Callee expression family of a call site evaluated as an l-value.
 */
enum class LValueCalleeKind : uint8_t
{
    None,       ///< Unrecognized or missing callee.
    Identifier, ///< Simple unqualified identifier or tokFunction.
    Member,     ///< Member access call (e.g. object.method()).
    Scoped      ///< Qualified scoped identifier call (e.g. namespace::func()).
};

/**
 * @brief One branch of a ternary expression evaluated as an l-value candidate.
 */
struct LValueBranchFact
{
    IndexedByteRange bytes;     ///< Byte span of the branch expression.
    SourceRange range;          ///< Line and column span of the branch expression.
    std::string_view text;      ///< Trimmed source spelling of the branch.
    bool isConsequence = false; ///< True for consequence branch; false for alternative branch.
};

/**
 * @brief Call expression evaluated as an l-value on the left side of an assignment.
 */
struct LValueCallSiteFact
{
    IndexedByteRange bytes;                               ///< Byte span of the call expression.
    SourceRange range;                                    ///< Line and column span of the call expression.
    LValueCalleeKind calleeKind = LValueCalleeKind::None; ///< Callee syntactic category.
    IndexedByteRange calleeBytes;                         ///< Byte span of the callee node.
    SourceRange calleeRange;                              ///< Line and column span of the callee node.
    std::string_view calleeText;                          ///< Spelling of callee identifier or scoped name.
    IndexedByteRange objectBytes;                         ///< Member callee: byte span of receiver object.
    SourceRange objectRange;                              ///< Member callee: line and column span of receiver object.
    std::string_view objectText;                          ///< Member callee: source spelling of receiver object.
    IndexedByteRange memberBytes;                         ///< Member callee: byte span of member identifier.
    SourceRange memberRange;                              ///< Member callee: line and column span of member identifier.
    std::string_view memberName;                          ///< Member callee: spelling of member method.
};

/**
 * @brief One expression node evaluated for modifiable l-value assignability.
 */
struct LValueCandidateFact
{
    IndexedByteRange bytes;                               ///< Byte span of the expression (including outer parens).
    SourceRange range;                                    ///< Line and column span of the expression.
    IndexedByteRange unwrappedBytes;                      ///< Byte span after unwrapping all outer parentheses.
    SourceRange unwrappedRange;                           ///< Line and column span after unwrapping outer parens.
    std::string_view text;                                ///< Source spelling of the unwrapped node.
    LValueCandidateKind kind = LValueCandidateKind::None; ///< Evaluated l-value candidate shape.
    bool isParenthesized = false;                         ///< Whether outer parentheses were present.
    bool isBareDataType = false;                          ///< Whether the unwrapped spelling is a primitive type.

    std::string_view name; ///< Identifier or member name.
    uint32_t refByte = 0;  ///< Start byte offset of identifier for scope lookup.

    IndexedByteRange objectBytes; ///< Member: receiver object byte span.
    SourceRange objectRange;      ///< Member: receiver object line and column span.
    std::string_view objectText;  ///< Member: receiver object source spelling.
    IndexedByteRange memberBytes; ///< Member: member identifier byte span.
    SourceRange memberRange;      ///< Member: member identifier line and column span.
    std::string_view memberName;  ///< Member: member identifier spelling.

    IndexedByteRange operandBytes; ///< Unary @: inner operand byte span.
    SourceRange operandRange;      ///< Unary @: inner operand line and column span.
    std::string_view operandText;  ///< Unary @: inner operand source spelling.

    IndexedByteRange consequenceBytes; ///< Ternary: consequence branch byte span.
    SourceRange consequenceRange;      ///< Ternary: consequence branch line and column span.
    std::string_view consequenceText;  ///< Ternary: consequence branch source spelling.
    IndexedByteRange alternativeBytes; ///< Ternary: alternative branch byte span.
    SourceRange alternativeRange;      ///< Ternary: alternative branch line and column span.
    std::string_view alternativeText;  ///< Ternary: alternative branch source spelling.
    uint32_t branchBegin = 0;          ///< First slice index into LValueSiteFacts::ternaryBranches.
    uint32_t branchCount = 0;          ///< Number of branches in ternaryBranches slice.

    bool isCall = false;        ///< Unwrapped expression is a call expression.
    IndexedByteRange callBytes; ///< Byte span of the call expression if isCall.
};

/**
 * @brief Plain struct holding receiver object and member identifier details.
 */
struct LValueMemberParts
{
    IndexedByteRange objBytes; ///< Receiver object byte span.
    SourceRange objRange;      ///< Receiver object line and column span.
    std::string_view objText;  ///< Receiver object source spelling.
    IndexedByteRange memBytes; ///< Member identifier byte span.
    SourceRange memRange;      ///< Member identifier line and column span.
    std::string_view memText;  ///< Member identifier spelling.

    /**
     * @brief Copies member details to a candidate fact.
     * @param[out] f Target candidate fact.
     */
    void ApplyTo(LValueCandidateFact& f) const noexcept
    {
        f.objectBytes = objBytes;
        f.objectRange = objRange;
        f.objectText = objText;
        f.memberBytes = memBytes;
        f.memberRange = memRange;
        f.memberName = memText;
        f.name = memText;
    }

    /**
     * @brief Copies member details to a call site fact.
     * @param[out] f Target call site fact.
     */
    void ApplyTo(LValueCallSiteFact& f) const noexcept
    {
        f.objectBytes = objBytes;
        f.objectRange = objRange;
        f.objectText = objText;
        f.memberBytes = memBytes;
        f.memberRange = memRange;
        f.memberName = memText;
    }
};

/**
 * @brief An assignment expression whose left-hand target was evaluated for l-value validity.
 */
struct LValueAssignmentFact
{
    IndexedByteRange bytes;              ///< Byte span of the assignment expression.
    SourceRange range;                   ///< Line and column span of the assignment expression.
    IndexedByteRange targetBytes;        ///< Byte span of the left-hand target expression.
    SourceRange targetRange;             ///< Line and column span of the target expression.
    std::string_view targetText;         ///< Source spelling of the left-hand target expression.
    bool targetIsDirectCall = false;     ///< Top-level target node symbol is symCallExpression.
    bool targetIsUnwrappedCall = false;  ///< Target unwrapped from parentheses is symCallExpression.
    IndexedByteRange callBytes;          ///< Byte span of the call expression if target is a call.
    bool targetIsDirectTernary = false;  ///< Top-level target node symbol is symTernaryExpression.
    LValueCandidateFact targetCandidate; ///< Flattened candidate fact for the target node.
};

/**
 * @brief AST-free facts for assignment targets, call l-values, and candidate l-value expressions.
 * @note Text views point into the source passed to BuildLValueSiteFacts, which must outlive this object.
 */
struct LValueSiteFacts
{
    std::vector<LValueAssignmentFact> assignments; ///< Ordered by start byte, then by end byte descending.
    std::vector<LValueCallSiteFact> callSites;     ///< Ordered by start byte, then by end byte descending.
    std::vector<LValueCandidateFact> candidates;   ///< Ordered by start byte, then by end byte descending.
    std::vector<LValueBranchFact> ternaryBranches; ///< Ternary branches sliced by LValueCandidateFact.

    /**
     * @brief Finds the assignment fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the assignment expression.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const LValueAssignmentFact* FindAssignment(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(assignments, bytes);
    }

    /**
     * @brief Finds the call site fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the call expression.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const LValueCallSiteFact* FindCallSite(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(callSites, bytes);
    }

    /**
     * @brief Finds the candidate l-value fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the candidate expression.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const LValueCandidateFact* FindCandidate(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(candidates, bytes);
    }
};

/**
 * @brief Extracts every assignment target, l-value call site, and l-value candidate expression of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return LValue site facts ordered by source position for exact-span binary search.
 */
[[nodiscard]] LValueSiteFacts BuildLValueSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
