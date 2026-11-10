#pragma once

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/NullFlowFacts.h"
#include "analysis/index/ReturnSiteFacts.h"
#include "analysis/index/SpanFactSearch.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Syntactic classification of a control flow statement or block record.
 */
enum class ControlFlowRecordKind : uint8_t
{
    Other,
    Function,
    Lambda,
    Block,
    If,
    Switch,
    Case,
    While,
    DoWhile,
    For,
    Foreach,
    Return,
    Break,
    Continue,
    Try
};

/**
 * @brief Extracted case label within a switch statement or case clause.
 */
struct ControlFlowCaseLabelFact
{
    IndexedByteRange bytes;       ///< Byte span of the case label expression.
    SourceRange range;            ///< Line and column span of the case label expression.
    std::string_view text;        ///< Trimmed source text of the label expression.
    bool isUnusable = false;      ///< True for string literals or floating point numbers with a dot.
    bool isNumberLiteral = false; ///< True if expression is a number literal.
    bool isStringLiteral = false; ///< True if expression is a string literal.
    bool isIdentifier = false;    ///< True for identifier or scoped identifier.
    bool isCall = false;          ///< True for call expressions (illegal as case constant).
    bool isDefault = false;       ///< True if this represents a default clause.
};

/**
 * @brief Condition operand text and constant-ness for branch and loop statements.
 */
struct ControlFlowConditionFact
{
    IndexedByteRange bytes;     ///< Byte span of the condition expression.
    SourceRange range;          ///< Line and column span of the condition expression.
    std::string_view text;      ///< Source text of the condition expression.
    bool present = false;       ///< True if a condition expression was written.
    bool isConstant = false;    ///< True if the condition is a compile-time literal constant.
    bool constantValue = false; ///< Logical truth value when isConstant is true.
};

/**
 * @brief Empty statement branches on an if statement (`if (c);` or `else;`).
 */
struct ControlFlowBranchFact
{
    bool hasEmptyConsequence = false; ///< Consequence is a bare semicolon token.
    SourceRange consequenceRange;     ///< Range of the empty consequence semicolon.
    bool hasEmptyAlternative = false; ///< Alternative is a bare semicolon token.
    SourceRange alternativeRange;     ///< Range of the empty alternative semicolon.
};

/**
 * @brief Details of a return statement including bare returns and null literals.
 */
struct ControlFlowReturnFact
{
    bool hasValue = false;           ///< Return statement carries a value expression.
    bool isBare = false;             ///< Bare `return;` without any value.
    bool returnsNullLiteral = false; ///< Value expression is exactly the null literal.
    BinaryOperandFact valueOperand;  ///< Value operand details when hasValue is true.
};

/**
 * @brief Function or lambda declaration body and return expectations.
 */
struct ControlFlowFunctionFact
{
    IndexedByteRange bytes;            ///< Byte span of the declaration or lambda node.
    SourceRange range;                 ///< Line and column span of the declaration.
    std::string_view name;             ///< Declared function name (empty for lambdas).
    SourceRange nameRange;             ///< Line and column span of function name.
    std::string_view returnType;       ///< Declared return type spelling.
    SourceRange returnTypeRange;       ///< Line and column span of return type.
    IndexedByteRange bodyBytes;        ///< Byte span of the statement body block.
    SourceRange bodyRange;             ///< Line and column span of the statement body block.
    bool isLambda = false;             ///< True for lambda expressions.
    bool implicitVoid = false;         ///< True for constructors and destructors.
    bool definitelyReturns = false;    ///< Pass-1 definite return result from NodeIndex.
    bool hasSyntaxError = false;       ///< True if the body contains a syntax error.
    uint32_t recordIndex = UINT32_MAX; ///< Corresponding record index in ControlFlowFacts::records.
};

/**
 * @brief Flat pre-order record for a statement, loop, branch, or callable body.
 */
struct ControlFlowRecord
{
    ControlFlowRecordKind kind = ControlFlowRecordKind::Other;
    IndexedByteRange bytes;            ///< Byte span of this statement or block.
    SourceRange range;                 ///< Line and column span of this statement or block.
    uint32_t parentIndex = UINT32_MAX; ///< Enclosing record index in ControlFlowFacts::records.
    uint32_t childBegin = 0;           ///< First direct child index into ControlFlowFacts::childIndices.
    uint32_t childCount = 0;           ///< Number of direct child records.
    uint32_t loopDepth = 0;            ///< Loop nesting depth at this statement.
    uint32_t switchDepth = 0;          ///< Switch nesting depth at this statement.

    std::string_view enclosingReturnType;   ///< Return type of the enclosing function.
    std::string_view enclosingFunctionName; ///< Name of the enclosing function.
    bool enclosingImplicitVoid = false;     ///< True if enclosing function is constructor/destructor.

    ControlFlowConditionFact condition; ///< Condition details for if and loop statements.
    ControlFlowBranchFact branch;       ///< Empty branch flags for if statements.

    uint32_t labelBegin = 0; ///< First label index into ControlFlowFacts::labels.
    uint32_t labelCount = 0; ///< Number of labels in this switch statement.

    ControlFlowReturnFact returnFact; ///< Details for return statements.

    uint32_t functionIndex = UINT32_MAX; ///< Index into ControlFlowFacts::functions for functions/lambdas.
};

/**
 * @brief AST-free control flow facts of one document revision.
 * @note Text views point into the source passed to BuildControlFlowFacts, which must outlive this object.
 */
struct ControlFlowFacts
{
    std::vector<ControlFlowRecord> records;         ///< Pre-order statements and callable bodies.
    std::vector<uint32_t> childIndices;             ///< Direct child record indices sliced by childBegin/childCount.
    std::vector<ControlFlowCaseLabelFact> labels;   ///< Case labels sliced by labelBegin/labelCount.
    std::vector<ControlFlowFunctionFact> functions; ///< Functions and lambdas ordered by start byte.
    ReturnSiteFacts returnSites;                    ///< Valued return statements with owner resolution.

    /**
     * @brief Finds the control flow record that spans exactly the given bytes.
     * @param[in] bytes Byte span of the statement or block node.
     * @return Matching record pointer, or nullptr if not found.
     */
    [[nodiscard]] inline const ControlFlowRecord* FindRecord(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(records, bytes);
    }

    /**
     * @brief Finds the function fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the function or lambda node.
     * @return Matching function fact pointer, or nullptr if not found.
     */
    [[nodiscard]] inline const ControlFlowFunctionFact* FindFunction(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(functions, bytes);
    }

    /**
     * @brief Finds the case label fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the case label expression node.
     * @return Matching label fact pointer, or nullptr if not found.
     */
    [[nodiscard]] inline const ControlFlowCaseLabelFact* FindLabel(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(labels, bytes);
    }

    /**
     * @brief Returns a slice of direct child record indices for a parent record.
     * @param[in] record Parent control flow record.
     * @return Contiguous span of record indices.
     */
    [[nodiscard]] inline std::span<const uint32_t> ChildrenOf(const ControlFlowRecord& record) const noexcept
    {
        return record.childBegin < childIndices.size()
                   ? std::span<const uint32_t>(childIndices.data() + record.childBegin, record.childCount)
                   : std::span<const uint32_t>{};
    }

    /**
     * @brief Returns a slice of case label facts for a switch statement record.
     * @param[in] record Switch statement control flow record.
     * @return Contiguous span of case label facts.
     */
    [[nodiscard]] inline std::span<const ControlFlowCaseLabelFact>
    LabelsOf(const ControlFlowRecord& record) const noexcept
    {
        return record.labelBegin < labels.size()
                   ? std::span<const ControlFlowCaseLabelFact>(labels.data() + record.labelBegin, record.labelCount)
                   : std::span<const ControlFlowCaseLabelFact>{};
    }
};

/**
 * @brief Extracts all AST-free control flow facts across functions and statements in the document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text buffer; must outlive returned facts.
 * @return Populated control flow facts structure.
 */
[[nodiscard]] ControlFlowFacts BuildControlFlowFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
