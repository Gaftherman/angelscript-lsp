#pragma once

#include "features/formatting/FormattingTypes.h"
#include <optional>
#include <string_view>

namespace angel_lsp::features
{
/**
 * @brief Applies a named BasedOnStyle preset (LLVM, Google, Chromium, Mozilla, WebKit, Microsoft, GNU).
 * @param[in,out] style Style configuration to populate.
 * @param[in] preset Preset identifier.
 */
void ApplyBasedOnStylePreset(ClangFormatStyle& style, std::string_view preset);

/**
 * @brief Parses a boolean literal from YAML text.
 * @param[in] val String view value.
 * @return Parsed boolean or std::nullopt.
 */
std::optional<bool> ParseClangBool(std::string_view val);

/**
 * @brief Parses an unsigned 32-bit integer from YAML text.
 * @param[in] val String view value.
 * @return Parsed uint32_t or std::nullopt.
 */
std::optional<uint32_t> ParseClangUInt(std::string_view val);

/**
 * @brief Parses a signed 32-bit integer from YAML text.
 * @param[in] val String view value.
 * @return Parsed int32_t or std::nullopt.
 */
std::optional<int32_t> ParseClangInt(std::string_view val);

/**
 * @brief Parses BreakBeforeBraces value into BraceStyle.
 * @param[in] val String view value.
 * @return Parsed BraceStyle or std::nullopt.
 */
std::optional<BraceStyle> ParseClangBraceStyle(std::string_view val);

/**
 * @brief Parses PointerAlignment value.
 * @param[in] val String view value.
 * @return Parsed PointerAlignment or std::nullopt.
 */
std::optional<PointerAlignment> ParseClangPointerAlignment(std::string_view val);

/**
 * @brief Parses ReferenceAlignment value.
 * @param[in] val String view value.
 * @return Parsed ReferenceAlignment or std::nullopt.
 */
std::optional<ReferenceAlignment> ParseClangReferenceAlignment(std::string_view val);

/**
 * @brief Parses SpaceBeforeParens value.
 * @param[in] val String view value.
 * @return Parsed SpaceBeforeParensStyle or std::nullopt.
 */
std::optional<SpaceBeforeParensStyle> ParseClangSpaceBeforeParens(std::string_view val);

/**
 * @brief Parses UseTab value.
 * @param[in] val String view value.
 * @return Parsed UseTabStyle or std::nullopt.
 */
std::optional<UseTabStyle> ParseClangUseTab(std::string_view val);

/**
 * @brief Parses AllowShortBlocksOnASingleLine value.
 * @param[in] val String view value.
 * @return Parsed ShortBlockStyle or std::nullopt.
 */
std::optional<ShortBlockStyle> ParseClangShortBlockStyle(std::string_view val);

/**
 * @brief Parses AllowShortFunctionsOnASingleLine value.
 * @param[in] val String view value.
 * @return Parsed ShortFunctionStyle or std::nullopt.
 */
std::optional<ShortFunctionStyle> ParseClangShortFunctionStyle(std::string_view val);

/**
 * @brief Parses NamespaceIndentation value.
 * @param[in] val String view value.
 * @return Parsed NamespaceIndentationStyle or std::nullopt.
 */
std::optional<NamespaceIndentationStyle> ParseClangNamespaceIndentation(std::string_view val);
} // namespace angel_lsp::features
