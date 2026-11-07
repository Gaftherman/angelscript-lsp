#pragma once

#include "analysis/formatting/FormattingTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace angel_lsp::analysis::formatting
{

/**
 * @brief One replacement produced by the formatter, in 0-based line and column coordinates.
 */
struct FormattedEdit
{
    uint32_t startLine = 0;   ///< First replaced line.
    uint32_t startColumn = 0; ///< Column on `startLine` where the replacement begins.
    uint32_t endLine = 0;     ///< Last replaced line.
    uint32_t endColumn = 0;   ///< Column on `endLine` where the replacement ends (exclusive).
    std::string newText;      ///< Formatted text replacing the range.
};

/**
 * @brief Formats a whole source text: tokenize, build lines, render, collapse blank lines, reassemble.
 * @param[in] sourceCode Text to format; a leading UTF-8 BOM is kept.
 * @param[in] formatOptions Layout and style options.
 * @return Formatted text; empty for empty input; the input unchanged when formatting is disabled.
 */
[[nodiscard]] std::string FormatSourceCode(std::string_view sourceCode, const FormatCodeOptions& formatOptions);

/**
 * @brief Formats a whole source text with the editor layout settings and the main style switches only.
 * @param[in] sourceCode Text to format.
 * @param[in] options Editor whitespace settings.
 * @param[in] braceStyle Where a block's opening brace goes; value braces ignore it.
 * @param[in] spacesInsideParentheses Whether to pad the inside of parentheses.
 * @return Formatted text, as the `FormatCodeOptions` overload returns it.
 */
[[nodiscard]] std::string FormatSourceCode(std::string_view sourceCode, const FormatLayoutOptions& options,
                                           BraceStyle braceStyle = BraceStyle::Allman,
                                           bool spacesInsideParentheses = false);

/**
 * @brief Applies the nearest `.clang-format` above the document on top of the editor options.
 * @param[in] uri Document URI or path used to search for `.clang-format`.
 * @param[in] opts Options built from the editor settings.
 * @return Options with the file style applied, or `opts` unchanged when no style file is found.
 * @note Cost: a directory walk toward the root plus one file read when a style file exists.
 */
[[nodiscard]] FormatCodeOptions ResolveEffectiveFormatOptions(std::string_view uri, FormatCodeOptions opts);

/**
 * @brief Formats the whole document and expresses the result as one replacement of the full text.
 * @param[in] sourceCode Document text, not empty.
 * @param[in] opts Effective options (style file already applied).
 * @return Replacement spanning the whole document, or nullopt when formatting changes nothing.
 */
[[nodiscard]] std::optional<FormattedEdit> FormatWholeDocument(std::string_view sourceCode,
                                                               const FormatCodeOptions& opts);

/**
 * @brief Formats the document and keeps only the formatted lines that cover the requested line range.
 * @param[in] sourceCode Document text, not empty.
 * @param[in] startLine First requested line, clamped to the document.
 * @param[in] endLine Last requested line, clamped to the document.
 * @param[in] opts Effective options (style file already applied).
 * @return Replacement of the whole lines that were matched, the whole-document edit when the range covers the
 *         full text, or nullopt when nothing changes or the range cannot be matched.
 */
[[nodiscard]] std::optional<FormattedEdit> FormatLineRange(std::string_view sourceCode, uint32_t startLine,
                                                           uint32_t endLine, const FormatCodeOptions& opts);

} // namespace angel_lsp::analysis::formatting
