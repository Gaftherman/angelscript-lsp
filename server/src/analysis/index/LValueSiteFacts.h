#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"

#include <cstdint>
#include <span>
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
    Identifier,       ///< Identifier, scoped identifier, tokFunction, or this expression.
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
    IndexedByteRange bytes;         ///< Byte span of the branch expression.
    bool isConsequence : 1 = false; ///< True for consequence branch; false for alternative branch.
};
static_assert(sizeof(LValueBranchFact) <= 12);

/**
 * @brief Receiver object and member identifier byte spans for a member l-value candidate.
 */
struct LValueMemberCandidateFact
{
    IndexedByteRange objectBytes; ///< Receiver object byte span.
    IndexedByteRange memberBytes; ///< Member identifier byte span.
};
static_assert(sizeof(LValueMemberCandidateFact) <= 16);

/**
 * @brief Call expression evaluated as an l-value on the left side of an assignment.
 */
struct LValueCallSiteFact
{
    IndexedByteRange bytes;                               ///< Byte span of the call expression.
    SourceRange range;                                    ///< Line and column span of the call expression.
    IndexedByteRange calleeBytes;                         ///< Byte span of the callee node.
    IndexedByteRange objectBytes;                         ///< Member callee: byte span of receiver object.
    IndexedByteRange memberBytes;                         ///< Member callee: byte span of member identifier.
    LValueCalleeKind calleeKind = LValueCalleeKind::None; ///< Callee syntactic category.
};
static_assert(sizeof(LValueCallSiteFact) <= 52);

/**
 * @brief Compact expression node evaluated for modifiable l-value assignability.
 */
struct LValueCandidateFact
{
    IndexedByteRange bytes;          ///< Byte span of the expression (including outer parens).
    IndexedByteRange unwrappedBytes; ///< Byte span after unwrapping all outer parentheses.
    uint32_t detailIndex = 0;        ///< Pool index into ternaryBranches, members, or unaryOperands.
    uint16_t detailCount = 0;        ///< Number of pool entries owned by this candidate.
    LValueCandidateKind kind = LValueCandidateKind::None; ///< Evaluated l-value candidate shape.
    bool isParenthesized : 1 = false;                     ///< True when outer parentheses surrounded the expression.
};
static_assert(sizeof(LValueCandidateFact) <= 24);

/**
 * @brief An assignment expression whose left-hand target was evaluated for l-value validity.
 */
struct LValueAssignmentFact
{
    IndexedByteRange bytes;                 ///< Byte span of the assignment expression.
    SourceRange range;                      ///< Line and column span of the assignment expression.
    SourceRange targetRange;                ///< Line and column span of the target expression.
    LValueCandidateFact targetCandidate;    ///< Flattened candidate fact for the target node.
    bool targetIsDirectCall : 1 = false;    ///< Top-level target node symbol is symCallExpression.
    bool targetIsUnwrappedCall : 1 = false; ///< Target unwrapped from parentheses is symCallExpression.
    bool targetIsDirectTernary : 1 = false; ///< Top-level target node symbol is symTernaryExpression.
};
static_assert(sizeof(LValueAssignmentFact) <= 68);

/**
 * @brief AST-free facts for assignment targets, call l-values, and candidate l-value expressions.
 * @note Byte spans and TextOf() views refer to source passed to BuildLValueSiteFacts, which must outlive this object.
 */
struct LValueSiteFacts
{
    std::string_view source;                        ///< Document source text; must outlive this object.
    std::vector<LValueAssignmentFact> assignments;  ///< Ordered by start byte, then by end byte descending.
    std::vector<LValueCallSiteFact> callSites;      ///< Ordered by start byte, then by end byte descending.
    std::vector<LValueCandidateFact> candidates;    ///< Ordered by start byte, then by end byte descending.
    std::vector<LValueBranchFact> ternaryBranches;  ///< Ternary branches sliced by LValueCandidateFact.
    std::vector<LValueMemberCandidateFact> members; ///< Member receiver and member spans indexed by candidates.
    std::vector<IndexedByteRange> unaryOperands;    ///< Unary @ operand spans indexed by candidates.

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

    /**
     * @brief Returns the ternary branch slice owned by a Ternary candidate.
     * @param[in] candidate Candidate fact of kind Ternary.
     * @return Span of branch facts, or empty when absent or out of range.
     */
    [[nodiscard]] std::span<const LValueBranchFact> TernaryBranches(const LValueCandidateFact& candidate) const noexcept
    {
        if (candidate.kind != LValueCandidateKind::Ternary || candidate.detailCount == 0)
        {
            return {};
        }
        const size_t begin = candidate.detailIndex;
        const size_t count = candidate.detailCount;
        if (begin >= ternaryBranches.size() || count > ternaryBranches.size() - begin)
        {
            return {};
        }
        return std::span<const LValueBranchFact>(ternaryBranches.data() + begin, count);
    }

    /**
     * @brief Returns the receiver and member byte spans owned by a Member candidate.
     * @param[in] candidate Candidate fact of kind Member.
     * @return Member span pair, or default empty spans when absent.
     */
    [[nodiscard]] LValueMemberCandidateFact MemberOf(const LValueCandidateFact& candidate) const noexcept
    {
        if (candidate.kind != LValueCandidateKind::Member || candidate.detailCount == 0 ||
            candidate.detailIndex >= members.size())
        {
            return {};
        }
        return members[candidate.detailIndex];
    }

    /**
     * @brief Returns the inner operand byte span owned by a UnaryDereference candidate.
     * @param[in] candidate Candidate fact of kind UnaryDereference.
     * @return Operand byte span, or empty range when absent.
     */
    [[nodiscard]] IndexedByteRange UnaryOperandBytes(const LValueCandidateFact& candidate) const noexcept
    {
        if (candidate.kind != LValueCandidateKind::UnaryDereference || candidate.detailCount == 0 ||
            candidate.detailIndex >= unaryOperands.size())
        {
            return {};
        }
        return unaryOperands[candidate.detailIndex];
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
