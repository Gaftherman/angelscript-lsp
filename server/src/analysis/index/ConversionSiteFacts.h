#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief One expression child of a conversion site, with the spans and spelling the checkers read.
 */
struct ConversionExpressionFact
{
    IndexedByteRange bytes; ///< Byte span; keys the resolution overloads and conditional-statement lookup.
    SourceRange range;      ///< Line and column span used for diagnostics and scope lookup.
    std::string_view text;  ///< Source spelling of the node; views the analysed document text.

    /**
     * @brief Reports whether the syntax child was present.
     * @return True when the fact spans at least one byte; absent children leave the fact default-constructed.
     */
    [[nodiscard]] bool Present() const noexcept
    {
        return !bytes.Empty();
    }
};

/**
 * @brief An if, while, do-while, for or ternary node whose condition is checked for boolean conversion.
 */
struct ConditionSiteFact
{
    IndexedByteRange bytes;             ///< Byte span of the whole statement or ternary expression.
    SourceRange statementRange;         ///< Line and column span; its start selects the lexical scope.
    ConversionExpressionFact condition; ///< Condition expression, after the expression-statement unwrap.
    ConversionExpressionFact unwrapped; ///< The condition with parentheses removed; absent when none remains.
    uint32_t operandBegin = 0;          ///< First slice index into ConversionSiteFacts::booleanOperands.
    uint32_t operandCount = 0;          ///< Number of leaf operands, in left-to-right source order.
    bool isTernary = false;             ///< The site is a ternary expression, which skips the handle check.
    bool isRootIf = false;              ///< An if statement that is not the else branch of another one.
};

/**
 * @brief A ternary expression with both branches present.
 */
struct TernarySiteFact
{
    IndexedByteRange bytes;               ///< Byte span of the ternary expression.
    SourceRange range;                    ///< Line and column span; its start selects the lexical scope.
    ConversionExpressionFact consequence; ///< Branch taken when the condition holds.
    ConversionExpressionFact alternative; ///< Branch taken otherwise.
};

/**
 * @brief An expression statement that has an expression child.
 */
struct ExpressionStatementSiteFact
{
    IndexedByteRange bytes;              ///< Byte span of the statement.
    SourceRange statementRange;          ///< Line and column span; its start selects the lexical scope.
    ConversionExpressionFact expression; ///< First named child of the statement.
};

/**
 * @brief One foreach loop variable.
 */
struct ForeachVariableFact
{
    uint32_t ordinal = 0;  ///< Position among the foreach variables; selects `opForValueN`.
    std::string_view name; ///< Variable name; empty when the name child is absent.
    SourceRange nameRange; ///< Span of the name; its start selects the body scope.
};

/**
 * @brief A foreach statement whose collection is present.
 */
struct ForeachSiteFact
{
    IndexedByteRange bytes;              ///< Byte span of the statement.
    SourceRange statementRange;          ///< Line and column span; its start selects the lexical scope.
    ConversionExpressionFact collection; ///< Expression being iterated.
    uint32_t variableBegin = 0;          ///< First slice index into ConversionSiteFacts::foreachVariables.
    uint32_t variableCount = 0;          ///< Number of loop variables, in source order.
};

/**
 * @brief One declarator of an `auto` variable declaration.
 */
struct AutoDeclaratorFact
{
    SourceRange declaratorRange;    ///< Span of the declarator; reported when the initializer is missing.
    std::string_view name;          ///< Declared variable name.
    SourceRange nameRange;          ///< Span of the name; matched against the local definition position.
    ConversionExpressionFact value; ///< Initializer expression; absent when the declarator has none.
};

/**
 * @brief A variable declaration whose declared type is `auto`.
 */
struct AutoDeclarationSiteFact
{
    IndexedByteRange bytes;       ///< Byte span of the variable declaration.
    uint32_t declaratorBegin = 0; ///< First slice index into ConversionSiteFacts::autoDeclarators.
    uint32_t declaratorCount = 0; ///< Number of declarators, in source order.
};

/**
 * @brief AST-free condition, ternary, expression-statement, foreach and `auto` declaration sites of one revision.
 * @note Text views point into the source passed to BuildConversionSiteFacts, which must outlive this object.
 */
struct ConversionSiteFacts
{
    std::vector<ConditionSiteFact> conditions;             ///< Ordered by start byte, then by end byte descending.
    std::vector<ConversionExpressionFact> booleanOperands; ///< Leaf operands, sliced by ConditionSiteFact.
    std::vector<TernarySiteFact> ternaries;                ///< Ordered like conditions.
    std::vector<ExpressionStatementSiteFact> expressionStatements; ///< Ordered like conditions.
    std::vector<ForeachSiteFact> foreachLoops;                     ///< Ordered like conditions.
    std::vector<ForeachVariableFact> foreachVariables;             ///< Loop variables, sliced by ForeachSiteFact.
    std::vector<AutoDeclarationSiteFact> autoDeclarations;         ///< Ordered like conditions.
    std::vector<AutoDeclaratorFact> autoDeclarators;               ///< Declarators, sliced by AutoDeclarationSiteFact.

    /**
     * @brief Finds the condition site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the statement or ternary expression.
     * @return The fact, or nullptr when the span is not a condition site.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const ConditionSiteFact* FindCondition(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the ternary site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the ternary expression.
     * @return The fact, or nullptr when the span is not a complete ternary expression.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const TernarySiteFact* FindTernary(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the expression statement that spans exactly the given bytes.
     * @param[in] bytes Byte span of the statement.
     * @return The fact, or nullptr when the span is not an expression statement with an expression.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const ExpressionStatementSiteFact* FindExpressionStatement(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the foreach site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the foreach statement.
     * @return The fact, or nullptr when the span is not a foreach statement with a collection.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const ForeachSiteFact* FindForeach(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the `auto` declaration that spans exactly the given bytes.
     * @param[in] bytes Byte span of the variable declaration.
     * @return The fact, or nullptr when the span is not an `auto` variable declaration.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const AutoDeclarationSiteFact* FindAutoDeclaration(IndexedByteRange bytes) const noexcept;
};

/**
 * @brief Extracts every conversion site of the document that the type conversion checker visits.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return Facts ordered by start byte.
 */
[[nodiscard]] ConversionSiteFacts BuildConversionSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
