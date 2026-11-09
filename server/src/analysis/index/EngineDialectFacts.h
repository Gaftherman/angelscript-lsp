#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Source coordinates and byte range of an indexed AST node or token.
 */
struct IndexedSourceSpan
{
    /// Byte range within the source buffer.
    IndexedByteRange bytes{};
    /// 0-based starting line number.
    uint32_t startRow = 0;
    /// 0-based starting column offset.
    uint32_t startColumn = 0;
    /// 0-based ending line number.
    uint32_t endRow = 0;
    /// 0-based ending column offset.
    uint32_t endColumn = 0;

    [[nodiscard]] constexpr bool operator==(const IndexedSourceSpan& other) const noexcept = default;
};

/**
 * @brief Facts for a variable declarator initialized with a character literal.
 */
struct IndexedCharLiteralDeclarator
{
    /// Byte range of the declared variable type specifier.
    IndexedByteRange declaredTypeRange{};
    /// Source span of the character literal value expression.
    IndexedSourceSpan valueSpan{};

    [[nodiscard]] constexpr bool operator==(const IndexedCharLiteralDeclarator& other) const noexcept = default;
};

/**
 * @brief Facts for a named argument invocation within an argument list.
 */
struct IndexedNamedArgument
{
    /// Source span of the argument assignment expression.
    IndexedSourceSpan argumentSpan{};
    /// Source span of the target parameter identifier.
    IndexedSourceSpan targetSpan{};
    /// Byte range of the target parameter name within source.
    IndexedByteRange nameRange{};

    [[nodiscard]] constexpr bool operator==(const IndexedNamedArgument& other) const noexcept = default;
};

/**
 * @brief Facts for an assignment expression.
 */
struct IndexedAssignmentExpression
{
    /// Source span of the complete assignment expression.
    IndexedSourceSpan expressionSpan{};
    /// Source span of the left-hand assignment target.
    IndexedSourceSpan targetSpan{};
    /// Byte range of the target identifier within source text.
    IndexedByteRange targetIdentifierRange{};

    [[nodiscard]] constexpr bool operator==(const IndexedAssignmentExpression& other) const noexcept = default;
};

/**
 * @brief AST-free extracted facts for all engine dialect validation checks.
 */
struct EngineDialectFacts
{
    /// Source spans of foreach statement nodes.
    std::vector<IndexedSourceSpan> foreachStatements;
    /// Source spans of empty initializer-list element commas.
    std::vector<IndexedSourceSpan> emptyListElements;
    /// Declarators initialized with character literal strings.
    std::vector<IndexedCharLiteralDeclarator> charLiteralDeclarators;
    /// Source spans of integer-by-integer division binary expressions.
    std::vector<IndexedSourceSpan> integerDivisions;
    /// Extracted named argument expressions and target names.
    std::vector<IndexedNamedArgument> namedArguments;
    /// Extracted assignment expressions with operator and target metadata.
    std::vector<IndexedAssignmentExpression> assignmentExpressions;
    /// Source spans of non-heredoc multiline string literals.
    std::vector<IndexedSourceSpan> multilineStrings;
};

/**
 * @brief Precalculated source coordinates and receiver facts for a scheduler call expression.
 */
struct SchedulerCallSite
{
    /// Source span of the entire call expression.
    IndexedSourceSpan call{};
    /// Text view of the callee receiver object, if present.
    std::string_view receiverText{};
    /// Source spans of each evaluated argument expression.
    std::vector<IndexedSourceSpan> args;
};

/**
 * @brief Extracts the receiver object text view from a call expression AST node.
 * @param[in] callNode AST node representing the call expression.
 * @param[in] sourceCode Document source text buffer.
 * @return Receiver object text view, or empty if the call has no object receiver.
 */
[[nodiscard]] std::string_view ExtractReceiverText(TSNode callNode, std::string_view sourceCode) noexcept;

/**
 * @brief Builds AST-free facts for a scheduler call expression and its argument nodes.
 * @param[in] callNode AST node representing the call expression.
 * @param[in] argNodes Span of AST nodes for call arguments.
 * @param[in] source Document source text buffer.
 * @return Scheduler call site facts including call span, receiver text, and argument spans.
 */
[[nodiscard]] SchedulerCallSite BuildSchedulerCallSite(TSNode callNode, std::span<const TSNode> argNodes,
                                                       std::string_view source);

/**
 * @brief Extracts AST-free facts for engine dialect checks in a single document-wide pass.
 * @param[in] index Precomputed AST node index of the document.
 * @param[in] source Document source text buffer.
 * @return Aggregated collection of dialect facts for second-pass rule checking.
 */
[[nodiscard]] EngineDialectFacts BuildEngineDialectFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
