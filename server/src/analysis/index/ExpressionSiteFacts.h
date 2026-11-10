#pragma once

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/PropertyAccessFacts.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/ValueExpressionFacts.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief A binary expression site with the spans needed for fact lookup and scope resolution.
 */
struct BinarySiteFact
{
    IndexedByteRange bytes; ///< Byte span of the binary expression; keys request.binaryFacts->Find.
    SourceRange range;      ///< Line and column span; its start selects the lexical scope.
};

/**
 * @brief Precomputed inputs for constant folding along a parenthesized expression chain.
 */
struct FoldedConstantFact
{
    IndexedByteRange bytes;        ///< Byte span of the operand before unwrapping.
    SourceRange range;             ///< Line and column span used for diagnostics.
    bool isNumericLiteral = false; ///< True when a numeric literal is reached through parentheses and unaries.
    std::string_view foldedName;   ///< Identifier name reached through parentheses; empty when none.
    uint32_t foldedByte = 0;       ///< Start byte of foldedName, used for scoped definition lookup.
    uint32_t chainBegin = 0;       ///< First slice index into ExpressionSiteFacts::unwrappedSpans.
    uint32_t chainCount = 0;       ///< Number of unwrapped parenthesized levels along the chain.
};

/**
 * @brief A unary or postfix expression site with its operator spelling and operand data.
 */
struct UnaryPostfixSiteFact
{
    IndexedByteRange bytes;              ///< Byte span of the unary or postfix expression.
    SourceRange range;                   ///< Line and column span; its start selects the lexical scope.
    std::string_view op;                 ///< Operator spelling (e.g. "++", "--", "@", "!", "-", "+", "~").
    SourceRange operatorRange;           ///< Line and column span of the operator token.
    bool isPostfix = false;              ///< True for postfix expression; false for unary expression.
    bool isIncrementOrDecrement = false; ///< True when operator is "++" or "--".
    bool isAddressOfHandle = false;      ///< True when operator is "@".
    bool hasOperand = false;             ///< True when an operand expression child is present.
    IndexedByteRange operandBytes;       ///< Byte span of the operand; passes to type and reference queries.
    SourceRange operandRange;            ///< Line and column span of the operand; passes to diagnostics.
    std::string_view operandText;        ///< Trimmed source spelling of the operand.
    BinaryOperandFact operand;           ///< Operand with its syntax flags and constant-folding data.
    ValueExpressionFact operandValue;    ///< Operand read as a value expression; input of ResolveValueType.
    FoldedConstantFact foldedOperand;    ///< Operand folded-constant data for constant checking.
    PropertyAccessFact operandProperty;  ///< Unwrapped operand, or its indexed object, as a property access.
    bool operandIsIndexed = false;       ///< True when the unwrapped operand is an index expression.
};

/**
 * @brief A member expression site with property access inspection inputs and context flags.
 */
struct MemberExpressionSiteFact
{
    IndexedByteRange bytes;            ///< Byte span of the member expression.
    SourceRange range;                 ///< Line and column span; its start selects the lexical scope.
    bool isCallCallee = false;         ///< True when the member expression is the callee of a call.
    bool isWriteOrMutation = false;    ///< True when the member is an assignment LHS or mutation operand.
    PropertyAccessFact propertyAccess; ///< Member expression read as a virtual property access.
};

/**
 * @brief An index expression site whose indexed object is inspected for virtual property access.
 */
struct IndexExpressionSiteFact
{
    IndexedByteRange bytes;            ///< Byte span of the index expression.
    SourceRange range;                 ///< Line and column span; its start selects the lexical scope.
    bool isWriteOrMutation = false;    ///< True when the index is an assignment LHS or mutation operand.
    PropertyAccessFact objectProperty; ///< Indexed object expression read as a virtual property access.
};

/**
 * @brief An explicit cast expression site with its target type and value operand.
 */
struct CastExpressionSiteFact
{
    IndexedByteRange bytes;         ///< Byte span of the cast expression.
    SourceRange range;              ///< Line and column span; its start selects the lexical scope.
    std::string_view targetType;    ///< Target type spelling as written in the type field.
    bool hasValue = false;          ///< True when the value child expression is present.
    IndexedByteRange valueBytes;    ///< Byte span of the value child expression.
    SourceRange valueRange;         ///< Line and column span of the value child expression.
    BinaryOperandFact valueOperand; ///< Value operand with its syntax flags and constant-folding data.
    ValueExpressionFact value;      ///< Value read as a value expression; input of ResolveValueType.
    FoldedConstantFact foldedValue; ///< Value folded-constant data for constant checking.
};

/**
 * @brief AST-free expression sites of one syntax revision for type conversion checking.
 * @note Text views point into the source passed to BuildExpressionSiteFacts, which must outlive this object.
 */
struct ExpressionSiteFacts
{
    std::vector<BinarySiteFact> binaryExpressions;             ///< Ordered by start byte.
    std::vector<UnaryPostfixSiteFact> unaryPostfixExpressions; ///< Ordered by start byte.
    std::vector<MemberExpressionSiteFact> memberExpressions;   ///< Ordered by start byte.
    std::vector<IndexExpressionSiteFact> indexExpressions;     ///< Ordered by start byte.
    std::vector<CastExpressionSiteFact> castExpressions;       ///< Ordered by start byte.
    std::vector<FoldedConstantFact> foldedConstants;           ///< Ordered by start byte.
    std::vector<IndexedByteRange> unwrappedSpans;              ///< Parenthesized spans, sliced by FoldedConstantFact.

    /**
     * @brief Finds the binary expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the binary expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const BinarySiteFact* FindBinary(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the unary or postfix expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the unary or postfix node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const UnaryPostfixSiteFact* FindUnaryPostfix(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the member expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the member expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const MemberExpressionSiteFact* FindMember(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the index expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the index expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const IndexExpressionSiteFact* FindIndex(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the cast expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the cast expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const CastExpressionSiteFact* FindCast(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the folded-constant fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the operand or expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const FoldedConstantFact* FindFoldedConstant(IndexedByteRange bytes) const noexcept;
};

/**
 * @brief Extracts AST-free expression facts for type conversion checking from a syntax index.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return Expression site facts ordered by start byte.
 */
[[nodiscard]] ExpressionSiteFacts BuildExpressionSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
