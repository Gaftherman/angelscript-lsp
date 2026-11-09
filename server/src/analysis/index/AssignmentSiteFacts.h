#pragma once

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/ConversionSiteFacts.h"
#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/PropertyAccessFacts.h"

#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Value assigned to a funcdef handle, after a leading `@` is removed.
 */
struct FuncdefValueFact
{
    bool present = false;                          ///< The value node exists; an absent value ends the funcdef check.
    std::string_view text;                         ///< Trimmed source spelling of the unwrapped value.
    bool isLambda = false;                         ///< The unwrapped value is a lambda expression.
    bool hasParameterList = false;                 ///< The lambda wrote a parameter list; an absent list is not empty.
    std::vector<LambdaParameter> lambdaParameters; ///< Parameters read from the list; lambda values only.
};

/**
 * @brief An assignment expression whose operands were captured on both sides.
 */
struct AssignmentSiteFact
{
    IndexedByteRange bytes;          ///< Byte span of the assignment expression.
    SourceRange range;               ///< Line and column span; its start selects the lexical scope.
    BinaryOperandFact left;          ///< Left operand.
    BinaryOperandFact right;         ///< Right operand with its constant-folding data.
    std::string_view op;             ///< Operator spelling; empty when the operator token is absent.
    bool opIsAssign = false;         ///< The operator is the plain `=` token.
    bool leftIsHandle = false;       ///< The left operand carries a unary `@`.
    bool rightIsHandle = false;      ///< The right operand carries a unary `@`.
    IndexedByteRange leftValue;      ///< Left operand without its unary `@`.
    IndexedByteRange rightValue;     ///< Right operand without its unary `@`.
    bool rightIsNull = false;        ///< The right operand is the null literal once parentheses are removed.
    FuncdefValueFact rightFuncdef;   ///< Right operand read as a funcdef value.
    PropertyAccessFact leftProperty; ///< Left operand, or its indexed object, read as a property access.
    bool leftIsIndex = false;        ///< The left operand is an index expression.
};

/**
 * @brief AST-free assignment expressions of one revision.
 * @note Views point into the source passed to BuildAssignmentSiteFacts, which must outlive this object.
 */
struct AssignmentSiteFacts
{
    std::vector<AssignmentSiteFact> assignments; ///< Ordered by start byte, then by end byte descending.

    /**
     * @brief Finds the assignment that spans exactly the given bytes.
     * @param[in] bytes Byte span of the assignment expression.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const AssignmentSiteFact* Find(IndexedByteRange bytes) const noexcept;
};

/**
 * @brief Converts a binary operand fact to the expression fact the data-type diagnostics take.
 * @param[in] operand Operand fact with spans and spelling.
 * @return Expression fact carrying the same spans and spelling.
 */
[[nodiscard]] inline ConversionExpressionFact ToConversionExpression(const BinaryOperandFact& operand) noexcept
{
    return ConversionExpressionFact{operand.bytes, operand.range, operand.text};
}

/**
 * @brief Reads the value of a funcdef assignment or initializer.
 * @param[in] valueNode Value expression; a null node yields a fact that is not present.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; the fact's views point into it.
 * @return Funcdef value fact with a leading `@` removed.
 */
[[nodiscard]] FuncdefValueFact ReadFuncdefValue(TSNode valueNode, const NodeIndex& index, std::string_view source);

/**
 * @brief Captures every assignment expression of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; the facts' views point into it.
 * @return Assignment facts sorted for exact-span lookup; assignments missing an operand are skipped.
 */
[[nodiscard]] AssignmentSiteFacts BuildAssignmentSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
