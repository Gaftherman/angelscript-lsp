#pragma once

#include "analysis/index/Symbol.h"
#include "i18n/i18n.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Pinpointed location and diagnostic text for a recovered syntax error.
 */
struct PinpointedSyntaxError
{
    SourceRange range;              ///< Target source range of the missing delimiter or unexpected token.
    std::string message;            ///< Formatted diagnostic message.
    uint32_t suppressStartByte = 0; ///< Start byte of cascaded errors suppressed by this recovery.
    uint32_t suppressEndByte = 0;   ///< End byte of cascaded errors suppressed by this recovery.
};

/**
 * @brief Scanner state while inspecting the true branch after `?` in a ternary expression.
 */
struct TernaryScanState
{
    int depth = 0;                ///< Nesting depth of parentheses, brackets, and braces.
    int nestedTernary = 0;        ///< Count of nested `?` operators not yet matched by `:`.
    bool expectingOperand = true; ///< True when the next primary token is expected after an operator.
};

/**
 * @brief Advances index past ASCII whitespace and AngelScript comments in text buffer.
 * @param[in] text Source text buffer.
 * @param[in,out] i Cursor index advanced past comments and whitespace.
 */
void SkipWhitespaceAndComments(std::string_view text, size_t& i);

/**
 * @brief Checks whether a character can appear in an AngelScript identifier or numeric token.
 * @param[in] c Character to inspect.
 * @return True if alphanumeric or underscore.
 */
bool IsIdentChar(char c) noexcept;

/**
 * @brief Advances index past a single, double, or triple-quoted AngelScript string literal.
 * @param[in] text Source text buffer.
 * @param[in,out] idx Cursor positioned at the opening quote on entry.
 */
void SkipStringLiteral(std::string_view text, size_t& idx);

/**
 * @brief Computes line and column coordinates at targetOffset by scanning newlines from baseNode.
 * @param[in] sourceCode Full document source text.
 * @param[in] baseNode AST node at or before targetOffset providing initial line/column coordinates.
 * @param[in] targetOffset Target byte offset within sourceCode.
 * @return Zero-based row and column point corresponding to targetOffset.
 */
TSPoint ComputeLocalPoint(std::string_view sourceCode, TSNode baseNode, size_t targetOffset);

/**
 * @brief Attempts to pinpoint common missing-delimiter syntax errors (`)`, ternary `:`, `catch`).
 * @param[in] node Leaf ERROR or MISSING AST node being reported.
 * @param[in] sourceCode Full document source text.
 * @param[in] i18n Optional localizer for formatted syntax messages.
 * @return Pinpointed range and message if matched, or std::nullopt to fall back to default reporting.
 */
std::optional<PinpointedSyntaxError> TryPinpointSyntaxError(TSNode node, std::string_view sourceCode,
                                                            const angel_lsp::i18n::I18n* i18n);

/**
 * @brief Scans a ternary expression missing its ':' separator and pinpoints the unexpected token.
 * @param[in] node Leaf ERROR AST node being reported.
 * @param[in] sourceCode Full document source text.
 * @param[in] i18n Optional localizer for formatted syntax messages.
 * @return Pinpointed range and message when a ternary expression lacks ':', or std::nullopt.
 */
std::optional<PinpointedSyntaxError> TryPinpointMissingTernaryColon(TSNode node, std::string_view sourceCode,
                                                                    const angel_lsp::i18n::I18n* i18n);

/**
 * @brief Constructs a single-line token range anchored by scanning coordinates from baseNode.
 * @param[in] sourceCode Full document source text.
 * @param[in] baseNode AST node at or before tokStart providing initial coordinates.
 * @param[in] tokStart Token start byte offset.
 * @param[in] tokEnd Token end byte offset.
 * @return Zero-based single-line SourceRange covering `[tokStart, tokEnd)`.
 */
SourceRange MakeSingleTokenRange(std::string_view sourceCode, TSNode baseNode, size_t tokStart, size_t tokEnd);

/**
 * @brief Returns node if its symbol matches targetSymbol, otherwise searches its direct children.
 * @param[in] node Parent or leaf AST node to inspect.
 * @param[in] targetSymbol Pre-resolved Tree-sitter symbol ID to match.
 * @return Matching AST node, or null TSNode when absent.
 */
TSNode FindChildOrParentSymbol(TSNode node, TSSymbol targetSymbol);

} // namespace angel_lsp::analysis
