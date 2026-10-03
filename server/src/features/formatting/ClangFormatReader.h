#pragma once

#include "features/formatting/FormattingTypes.h"
#include <optional>
#include <string_view>

namespace angel_lsp::features
{
/**
 * @brief Parses the contents of a .clang-format configuration string.
 * @param[in] content YAML configuration content.
 * @return Parsed ClangFormatStyle if successful, std::nullopt otherwise.
 */
std::optional<ClangFormatStyle> ParseClangFormat(std::string_view content);

/**
 * @brief Discovers and loads the nearest .clang-format file for a document path or URI.
 * @param[in] documentUriOrPath File path or file:// URI of the document.
 * @return Parsed style if a configuration file was discovered, std::nullopt otherwise.
 */
std::optional<ClangFormatStyle> LoadClangFormatForFile(std::string_view documentUriOrPath);

/**
 * @brief Applies discovered ClangFormatStyle options to a FormatCodeOptions bundle.
 * @param[in,out] options Options bundle modified in-place.
 * @param[in] style Parsed clang-format style configuration.
 */
void ApplyClangFormatStyle(FormatCodeOptions& options, const ClangFormatStyle& style);
} // namespace angel_lsp::features
