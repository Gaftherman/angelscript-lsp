#pragma once

#include "features/formatting/FormattingTypes.h"

#include <lsp/types.h>
#include <optional>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Converts the editor's LSP formatting options into the protocol-free layout settings.
 * @param[in] options Options received with a formatting request.
 * @return Layout settings with the same tab size, indentation kind and trimming switches.
 */
[[nodiscard]] analysis::formatting::FormatLayoutOptions ToLayoutOptions(const lsp::FormattingOptions& options);

/**
 * @brief Formats a whole document, after applying the nearest `.clang-format` to the request options.
 * @param[in] request Formatting request.
 * @return One edit replacing the full text; an empty list when the text is empty, formatting is disabled or nothing
 *         changes.
 */
std::optional<std::vector<lsp::TextEdit>> FormatDocument(const FormattingRequest& request);

/**
 * @brief Formats the whole lines covered by the requested range.
 * @param[in] request Range formatting request.
 * @return One edit over the matched lines, the whole-document edit when the range covers the full text, or an
 *         empty list when nothing changes.
 */
std::optional<std::vector<lsp::TextEdit>> FormatRange(const RangeFormattingRequest& request);

/**
 * @brief Formats the current line after `;` or `}` is typed, unless the edit sits in a string, a comment or a
 *        document with syntax errors.
 * @param[in] request On-type formatting request.
 * @return Edits for the current line; nullopt for other characters or a suppressed position.
 */
std::optional<std::vector<lsp::TextEdit>> FormatOnType(const OnTypeFormattingRequest& request);
} // namespace angel_lsp::features
