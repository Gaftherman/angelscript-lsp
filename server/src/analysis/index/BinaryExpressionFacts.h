#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Operator family of a binary expression, as the operator checkers distinguish them.
 */
enum class BinaryOperatorClass : uint8_t
{
    Other,
    Identity,   ///< `is` and `!is`.
    Equality,   ///< `==` and `!=`.
    Relational, ///< `<`, `<=`, `>`, `>=`.
    Arithmetic, ///< `+`, `-`, `*`, `/`, `%`, `**`.
    Bitwise     ///< `|`, `&`, `^`, `<<`, `>>`, `>>>`.
};

/**
 * @brief One side of a binary expression with the syntax flags the operator checkers read.
 */
struct BinaryOperandFact
{
    IndexedByteRange bytes;              ///< Byte span of the operand; keys the resolution overloads.
    SourceRange range;                   ///< Line and column span used for diagnostics.
    std::string_view text;               ///< Source spelling; views the analysed document text.
    std::string_view foldedName;         ///< Identifier left after unwrapping parentheses only; empty when none.
    uint32_t foldedByte = 0;             ///< Start byte of foldedName, used for scoped definition lookup.
    bool isNullLiteral = false;          ///< Operand is the null literal.
    bool isThis = false;                 ///< Operand is the `this` expression.
    bool hasHandleOperator = false;      ///< Operand carries a unary `@` operator.
    bool isExtractableCandidate = false; ///< Member, identifier, call, index or scoped identifier expression.
    bool isNumericLiteral = false;       ///< Literal reached through parentheses and unary operators only.
};

/**
 * @brief A binary expression whose operator and both operands were captured.
 */
struct BinaryExpressionFact
{
    IndexedByteRange bytes;    ///< Byte span of the whole expression.
    SourceRange operatorRange; ///< Line and column span of the operator token.
    std::string_view op;       ///< Operator spelling; views the analysed document text.
    BinaryOperatorClass opClass = BinaryOperatorClass::Other;
    BinaryOperandFact left;
    BinaryOperandFact right;
};

/**
 * @brief An if statement that is not the else branch of another one, with the conditions along its else-if chain.
 */
struct IfLadderFact
{
    IndexedByteRange rootIf;     ///< Byte span of the root if statement.
    uint32_t conditionBegin = 0; ///< First slice index into BinaryExpressionFacts::ladderConditions.
    uint32_t conditionCount = 0; ///< Number of conditions, in source order.
};

/**
 * @brief AST-free binary expression and if-ladder facts of one syntax revision.
 * @note Text views point into the source passed to BuildBinaryExpressionFacts, which must outlive this object.
 */
struct BinaryExpressionFacts
{
    std::vector<BinaryExpressionFact>
        expressions; ///< Complete expressions ordered by start byte, then by end byte descending.
    std::vector<IndexedByteRange> ladderConditions; ///< Condition spans, sliced by IfLadderFact.
    std::vector<IfLadderFact> ladders;              ///< Root if statements ordered by start byte.

    /**
     * @brief Finds the binary expression that spans exactly the given bytes.
     * @param[in] bytes Byte span of the expression.
     * @return The fact, or nullptr when the span is not a complete binary expression.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const BinaryExpressionFact* Find(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the ladder rooted at the if statement that spans exactly the given bytes.
     * @param[in] rootIf Byte span of the root if statement.
     * @return The ladder, or nullptr when the span is not a root if statement.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const IfLadderFact* FindLadder(IndexedByteRange rootIf) const noexcept;

    /**
     * @brief Finds the binary expressions whose span lies inside the given bytes.
     * @param[in] bytes Enclosing byte span, such as an if condition.
     * @return Contiguous expressions in source order; empty when none lie inside.
     * @note Cost O(log N + K) for K returned expressions.
     */
    [[nodiscard]] std::vector<const BinaryExpressionFact*> FindWithin(IndexedByteRange bytes) const;
};

/**
 * @brief Extracts every complete binary expression and every if ladder of the document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return Facts ordered by start byte.
 */
[[nodiscard]] BinaryExpressionFacts BuildBinaryExpressionFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
