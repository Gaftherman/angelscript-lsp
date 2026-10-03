#pragma once

#include "features/formatting/FormattingLineBuilder.h"
#include "features/formatting/FormattingTokens.h"
#include "features/formatting/FormattingTypes.h"
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Renders structured lines into formatted text lines according to configuration.
 * @param[in] lines Line specifications.
 * @param[in] tokens Annotated token stream.
 * @param[in] options Effective formatting options.
 * @return Vector of rendered lines.
 */
std::vector<std::string> RenderLines(const std::vector<LineInfo>& lines, const std::vector<Token>& tokens,
                                     const FormatCodeOptions& options);

/**
 * @brief Collapses consecutive blank lines exceeding the limit and trims trailing newlines.
 * @param[in,out] lines Rendered lines modified in-place.
 * @param[in] options Effective formatting options.
 */
void CollapseAndTrimLines(std::vector<std::string>& lines, const FormatCodeOptions& options);

/**
 * @brief Assembles output text with BOM and final newlines.
 * @param[in] bom Byte order mark string view.
 * @param[in] lines Processed lines.
 * @param[in] options Formatting options.
 * @return Final formatted source text.
 */
std::string AssembleFormattedText(std::string_view bom, const std::vector<std::string>& lines,
                                  const lsp::FormattingOptions& options);

/**
 * @brief Extracts UTF-8 Byte Order Mark (BOM) if present.
 * @param[in,out] sourceCode Input source code.
 * @return BOM string view if found, empty view otherwise.
 */
std::string_view ExtractBom(std::string_view& sourceCode);

/**
 * @brief Splits source text by newline characters.
 * @param[in] text Input text.
 * @param[in] keepTrailingEmpty True to retain trailing empty line after final newline.
 * @return Vector of line strings.
 */
std::vector<std::string> SplitLines(std::string_view text, bool keepTrailingEmpty);

/**
 * @brief Joins a range of lines into a single newline-separated string.
 * @param[in] lines Source lines.
 * @param[in] startLine Inclusive start index.
 * @param[in] endLine Inclusive end index.
 * @return Joined string.
 */
std::string JoinLines(const std::vector<std::string>& lines, uint32_t startLine, uint32_t endLine);
} // namespace angel_lsp::features
