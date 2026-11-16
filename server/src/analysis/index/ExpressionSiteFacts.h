#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/PropertyAccessFacts.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/ValueExpressionFacts.h"
#include "utils/PositionEncoding.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/// Sentinel pool index indicating that no side-vector entry is attached.
inline constexpr uint32_t k_noExpressionFactIndex = UINT32_MAX;

/**
 * @brief Classification of unary and postfix operator tokens.
 */
enum class UnaryPostfixOp : uint8_t
{
    None,       ///< Missing or unrecognized operator token.
    PlusPlus,   ///< `++`
    MinusMinus, ///< `--`
    At,         ///< `@`
    Bang,       ///< `!`
    Not,        ///< `not`
    Minus,      ///< `-`
    Plus,       ///< `+`
    Tilde       ///< `~`
};

/**
 * @brief Maps a unary or postfix operator enum value back to its source token spelling.
 * @param[in] op Operator token kind.
 * @return Static operator spelling view, or empty for UnaryPostfixOp::None.
 */
[[nodiscard]] constexpr std::string_view OperatorSpelling(UnaryPostfixOp op) noexcept
{
    switch (op)
    {
    case UnaryPostfixOp::PlusPlus:
        return "++";
    case UnaryPostfixOp::MinusMinus:
        return "--";
    case UnaryPostfixOp::At:
        return "@";
    case UnaryPostfixOp::Bang:
        return "!";
    case UnaryPostfixOp::Not:
        return "not";
    case UnaryPostfixOp::Minus:
        return "-";
    case UnaryPostfixOp::Plus:
        return "+";
    case UnaryPostfixOp::Tilde:
        return "~";
    case UnaryPostfixOp::None:
        return {};
    }
    return {};
}

/**
 * @brief A binary expression site with the byte span needed for fact lookup and scope resolution.
 */
struct BinarySiteFact
{
    IndexedByteRange bytes; ///< Byte span of the binary expression; keys request.binaryFacts->Find.
};

/**
 * @brief Precomputed inputs for constant folding along a parenthesized expression chain.
 */
struct FoldedConstantFact
{
    IndexedByteRange bytes;            ///< Byte span of the operand before unwrapping.
    IndexedByteRange foldedNameBytes;  ///< Byte span of identifier reached through parentheses; empty when none.
    uint32_t chainBegin = 0;           ///< First slice index into ExpressionSiteFacts::unwrappedSpans.
    uint16_t chainCount = 0;           ///< Number of unwrapped parenthesized levels along the chain.
    bool isNumericLiteral : 1 = false; ///< True when a numeric literal is reached through parentheses and unaries.
};

/**
 * @brief A unary or postfix expression site with its operator kind and operand metadata.
 */
struct UnaryPostfixSiteFact
{
    IndexedByteRange bytes;        ///< Byte span of the unary or postfix expression.
    IndexedByteRange operandBytes; ///< Byte span of the operand; passes to type and reference queries.
    uint32_t operandPropertyIndex = k_noExpressionFactIndex; ///< Index into ExpressionSiteFacts::propertyAccesses.
    UnaryPostfixOp op = UnaryPostfixOp::None;                ///< Operator token classification.
    bool isPostfix : 1 = false;              ///< True for postfix expression; false for unary expression.
    bool isIncrementOrDecrement : 1 = false; ///< True when operator is "++" or "--".
    bool isAddressOfHandle : 1 = false;      ///< True when operator is "@".
    bool hasOperand : 1 = false;             ///< True when an operand expression child is present.
    bool operandIsIndexed : 1 = false;       ///< True when the unwrapped operand is an index expression.
    bool operandIsNumber : 1 = false;        ///< True when the unwrapped operand evaluates to a number literal.
};

/**
 * @brief A member expression site with context flags and an index into propertyAccesses.
 */
struct MemberExpressionSiteFact
{
    IndexedByteRange bytes;                                 ///< Byte span of the member expression.
    uint32_t propertyAccessIndex = k_noExpressionFactIndex; ///< Index into ExpressionSiteFacts::propertyAccesses.
    bool isCallCallee : 1 = false;      ///< True when the member expression is the callee of a call.
    bool isWriteOrMutation : 1 = false; ///< True when the member is an assignment LHS or mutation operand.
};

/**
 * @brief An index expression site whose indexed object is inspected for virtual property access and opIndex.
 */
struct IndexExpressionSiteFact
{
    IndexedByteRange bytes;                                 ///< Byte span of the index expression.
    IndexedByteRange objectBytes;                           ///< Byte span of the indexed object expression.
    uint32_t objectPropertyIndex = k_noExpressionFactIndex; ///< Index into ExpressionSiteFacts::propertyAccesses.
    uint32_t argBegin = 0;                                  ///< First index into ExpressionSiteFacts::indexArgs.
    uint16_t argCount = 0;                                  ///< Number of index argument expressions.
    bool isWriteOrMutation : 1 = false; ///< True when the index is an assignment LHS or mutation operand.
    bool hasSyntaxError : 1 = false;    ///< True when the index expression contains a syntax error.
};

/**
 * @brief An explicit cast expression site with its target type span and pooled value expression index.
 */
struct CastExpressionSiteFact
{
    IndexedByteRange bytes;                        ///< Byte span of the cast expression.
    IndexedByteRange targetTypeBytes;              ///< Byte span of the target type field.
    uint32_t valueIndex = k_noExpressionFactIndex; ///< Index into ExpressionSiteFacts::castValues.
    bool hasValue : 1 = false;                     ///< True when the value child expression is present.
};

/**
 * @brief AST-free expression sites of one syntax revision for type conversion checking.
 * @note Byte spans, TextOf(), and RangeOf() refer to source passed to BuildExpressionSiteFacts, which must outlive
 * this.
 */
struct ExpressionSiteFacts
{
    std::string_view source;                                   ///< Document source text; must outlive this object.
    utils::LineIndex lineIndex;                                ///< Line start byte offsets for O(log L) RangeOf.
    std::vector<BinarySiteFact> binaryExpressions;             ///< Ordered by start byte.
    std::vector<UnaryPostfixSiteFact> unaryPostfixExpressions; ///< Ordered by start byte.
    std::vector<MemberExpressionSiteFact> memberExpressions;   ///< Ordered by start byte.
    std::vector<IndexExpressionSiteFact> indexExpressions;     ///< Ordered by start byte.
    std::vector<CastExpressionSiteFact> castExpressions;       ///< Ordered by start byte.
    std::vector<FoldedConstantFact> foldedConstants;           ///< Ordered by start byte.
    std::vector<IndexedByteRange> unwrappedSpans;              ///< Parenthesized spans, sliced by FoldedConstantFact.
    std::vector<IndexedByteRange> indexArgs;          ///< Index argument byte spans, sliced by IndexExpression.
    std::vector<PropertyAccessFact> propertyAccesses; ///< Side pool for unary/member/index property checks.
    std::vector<ValueExpressionFact> castValues;      ///< Side pool for cast value expressions.

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
     * @brief Derives a 0-based line/column SourceRange from a byte span using lineIndex.
     * @param[in] bytes Half-open byte span within source.
     * @return Line and column range, or zero range when empty or out of bounds.
     */
    [[nodiscard]] SourceRange RangeOf(IndexedByteRange bytes) const noexcept
    {
        if ((bytes.startByte == 0 && bytes.endByte == 0) || bytes.startByte > bytes.endByte ||
            bytes.endByte > source.size() || lineIndex.starts.empty())
        {
            return {};
        }
        const auto pointAt = [&](uint32_t offset) noexcept -> std::pair<uint32_t, uint32_t>
        {
            const auto it =
                std::upper_bound(lineIndex.starts.begin(), lineIndex.starts.end(), static_cast<size_t>(offset));
            const uint32_t line = static_cast<uint32_t>((it - lineIndex.starts.begin()) - 1);
            const uint32_t col = static_cast<uint32_t>(offset - lineIndex.starts[line]);
            return {line, col};
        };
        const auto [startLine, startCol] = pointAt(bytes.startByte);
        const auto [endLine, endCol] = pointAt(bytes.endByte);
        return SourceRange{startLine, startCol, endLine, endCol};
    }

    /**
     * @brief Finds the binary expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the binary expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const BinarySiteFact* FindBinary(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(binaryExpressions, bytes);
    }

    /**
     * @brief Finds the unary or postfix expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the unary or postfix node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const UnaryPostfixSiteFact* FindUnaryPostfix(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(unaryPostfixExpressions, bytes);
    }

    /**
     * @brief Finds the member expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the member expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const MemberExpressionSiteFact* FindMember(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(memberExpressions, bytes);
    }

    /**
     * @brief Finds the index expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the index expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const IndexExpressionSiteFact* FindIndex(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(indexExpressions, bytes);
    }

    /**
     * @brief Finds the cast expression site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the cast expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const CastExpressionSiteFact* FindCast(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(castExpressions, bytes);
    }

    /**
     * @brief Finds the folded-constant fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the operand or expression node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const FoldedConstantFact* FindFoldedConstant(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(foldedConstants, bytes);
    }

    /**
     * @brief Returns the property access fact attached to a unary or postfix site's operand.
     * @param[in] site Unary or postfix expression site fact.
     * @return Property access fact, or kind None when absent.
     */
    [[nodiscard]] PropertyAccessFact OperandPropertyOf(const UnaryPostfixSiteFact& site) const noexcept
    {
        return site.operandPropertyIndex < propertyAccesses.size() ? propertyAccesses[site.operandPropertyIndex]
                                                                   : PropertyAccessFact{};
    }

    /**
     * @brief Returns the property access fact attached to a member expression site.
     * @param[in] site Member expression site fact.
     * @return Property access fact, or kind None when absent.
     */
    [[nodiscard]] PropertyAccessFact PropertyAccessOf(const MemberExpressionSiteFact& site) const noexcept
    {
        return site.propertyAccessIndex < propertyAccesses.size() ? propertyAccesses[site.propertyAccessIndex]
                                                                  : PropertyAccessFact{};
    }

    /**
     * @brief Returns the property access fact attached to an index expression site's object.
     * @param[in] site Index expression site fact.
     * @return Property access fact, or kind None when absent.
     */
    [[nodiscard]] PropertyAccessFact ObjectPropertyOf(const IndexExpressionSiteFact& site) const noexcept
    {
        return site.objectPropertyIndex < propertyAccesses.size() ? propertyAccesses[site.objectPropertyIndex]
                                                                  : PropertyAccessFact{};
    }

    /**
     * @brief Returns the value expression fact attached to a cast expression site.
     * @param[in] site Cast expression site fact.
     * @return Value expression fact, or kind None when absent.
     */
    [[nodiscard]] ValueExpressionFact ValueOf(const CastExpressionSiteFact& site) const noexcept
    {
        return site.valueIndex < castValues.size() ? castValues[site.valueIndex] : ValueExpressionFact{};
    }

    /**
     * @brief Returns the index argument byte spans owned by an index expression site.
     * @param[in] site Index expression site fact.
     * @return Span of index argument byte ranges, or empty when absent or out of range.
     */
    [[nodiscard]] std::span<const IndexedByteRange> IndexArgsOf(const IndexExpressionSiteFact& site) const noexcept
    {
        if (site.argCount == 0 || site.argBegin >= indexArgs.size() || site.argCount > indexArgs.size() - site.argBegin)
        {
            return {};
        }
        return std::span<const IndexedByteRange>(indexArgs.data() + site.argBegin, site.argCount);
    }

    /**
     * @brief Returns the parenthesized byte spans owned by a folded-constant chain.
     * @param[in] fact Folded-constant fact.
     * @return Span of parenthesized byte ranges, or empty when absent or out of range.
     */
    [[nodiscard]] std::span<const IndexedByteRange> UnwrappedSpansOf(const FoldedConstantFact& fact) const noexcept
    {
        if (fact.chainCount == 0 || fact.chainBegin >= unwrappedSpans.size() ||
            fact.chainCount > unwrappedSpans.size() - fact.chainBegin)
        {
            return {};
        }
        return std::span<const IndexedByteRange>(unwrappedSpans.data() + fact.chainBegin, fact.chainCount);
    }
};

/**
 * @brief Extracts AST-free expression facts for type conversion checking from a syntax index.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return Expression site facts ordered by start byte.
 */
[[nodiscard]] ExpressionSiteFacts BuildExpressionSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
