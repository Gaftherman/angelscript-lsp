#pragma once

#include "features/formatting/FormattingTypes.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Formats an entire AngelScript document according to options and style.
 * @param request Immutable formatting request context.
 * @return List of TextEdits (or std::nullopt if formatting failed / no edits needed).
 */
std::optional<std::vector<lsp::TextEdit>> FormatDocument(const FormattingRequest& request);

/**
 * @brief Formats a specific range in an AngelScript document.
 * @param request Immutable range formatting request context.
 * @return List of TextEdits for the specified range.
 */
std::optional<std::vector<lsp::TextEdit>> FormatRange(const RangeFormattingRequest& request);

/**
 * @brief Formats code triggered on typing specific characters (;, }).
 * @param request Immutable on-type formatting context.
 * @return List of TextEdits for the formatted region.
 */
std::optional<std::vector<lsp::TextEdit>> FormatOnType(const OnTypeFormattingRequest& request);

/**
 * @brief Directly formats an AngelScript source code string with comprehensive options.
 * @param sourceCode Source text to format.
 * @param formatOptions Bundled formatting options.
 * @return Formatted source code string.
 */
std::string FormatSourceCode(std::string_view sourceCode, const FormatCodeOptions& formatOptions);

/**
 * @brief Directly formats an AngelScript source code string.
 * @param sourceCode Source text to format.
 * @param options Formatting configuration options.
 * @param braceStyle Where a block's opening brace goes. Value braces ignore this.
 * @param spacesInsideParentheses Whether to insert spaces inside parentheses.
 * @return Formatted source code string.
 */
std::string FormatSourceCode(std::string_view sourceCode, const lsp::FormattingOptions& options,
                             BraceStyle braceStyle = BraceStyle::Allman, bool spacesInsideParentheses = false);
} // namespace angel_lsp::features
