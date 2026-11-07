#pragma once

#include "analysis/index/InlayHintFacts.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Formats hover tooltip text for a parameter inlay hint.
 * @param[in] param Function parameter information.
 * @param[in] label Resolved parameter label.
 * @return Formatted tooltip markdown string.
 */
std::string FormatParameterTooltip(const ParameterInformation& param, std::string_view label);

/**
 * @brief Formats a parameter label part value with optional modifiers and truncation.
 * @param[in] label Base parameter label.
 * @param[in] prefix Modifier prefix (e.g. "&out ", "&inout ").
 * @param[in] maxLength Maximum length threshold (0 for none).
 * @return Formatted parameter label string.
 */
std::string FormatParameterLabel(std::string_view label, std::string_view prefix, size_t maxLength);

/**
 * @brief Formats label text for an omitted default argument hint.
 * @param[in] param Function parameter information.
 * @param[in] mode Inlay hint display mode.
 * @param[in] maxLength Maximum label length threshold (0 for unlimited).
 * @return Formatted label string.
 */
std::string FormatOmittedDefaultLabel(const ParameterInformation& param, config::OmittedDefaultArgumentsMode mode,
                                      size_t maxLength);

/**
 * @brief Formats an omitted default argument label with optional leading or trailing comma separators.
 * @param[in] param Function parameter information.
 * @param[in] mode Inlay hint display mode.
 * @param[in] maxLength Maximum label length threshold (0 for unlimited).
 * @param[in] prefixComma True if a leading comma and space should precede the label.
 * @param[in] suffixComma True if a trailing comma and space should follow the label.
 * @return Formatted label string with requested commas.
 */
std::string FormatOmittedDefaultArg(const ParameterInformation& param, config::OmittedDefaultArgumentsMode mode,
                                    size_t maxLength, bool prefixComma, bool suffixComma);

/**
 * @brief Formats a clickable Markdown link to navigate to a parameter definition location.
 * @param[in] uri Target document URI.
 * @param[in] line1Based 1-based target line coordinate.
 * @param[in] col1Based 1-based target character coordinate.
 * @param[in] i18n Optional localization instance.
 * @return Formatted Markdown navigation link.
 */
std::string FormatGoToDefinitionLink(std::string_view uri, uint32_t line1Based, uint32_t col1Based,
                                     const angel_lsp::i18n::I18n* i18n = nullptr);

/**
 * @brief Formats hover tooltip markdown for an omitted default parameter.
 * @param[in] param Callee parameter information.
 * @param[in] definitionLink Optional clickable navigation link markdown.
 * @param[in] i18n Optional localization instance.
 * @return Formatted tooltip markdown string.
 */
std::string FormatDefaultParamTooltip(const ParameterInformation& param, std::string_view definitionLink = {},
                                      const angel_lsp::i18n::I18n* i18n = nullptr);

/**
 * @brief Text components for an auto variable inlay hint.
 */
struct AutoVariableHintText
{
    std::string label;   ///< Formatted label text (e.g. ": int" or ": Foo@").
    std::string tooltip; ///< Formatted markdown tooltip (e.g. "Deduced type: int").
};

/**
 * @brief Deduces and formats both the label and tooltip for an auto variable inlay hint in a single pass.
 * @param[in] autoVar Auto variable fact record.
 * @param[in] exprCtx Semantic expression type context.
 * @param[in] i18n Optional localization instance.
 * @return Formatted hint texts, or std::nullopt if the type cannot be deduced or is suppressed.
 */
std::optional<AutoVariableHintText> FormatAutoVariableHint(const IndexedAutoDeclarator& autoVar,
                                                           const ExpressionTypeContext& exprCtx,
                                                           const angel_lsp::i18n::I18n* i18n = nullptr);

/**
 * @brief Formats the label text for a lambda return type inlay hint.
 * @param[in] lambda Lambda site fact record.
 * @param[in] exprCtx Semantic expression type context.
 * @return Formatted label string (e.g. ": int"), or empty string if no valid hint.
 */
std::string FormatLambdaReturnLabel(const IndexedLambdaSite& lambda, const ExpressionTypeContext& exprCtx);

/**
 * @brief Formats the label text for an enum member implicit value hint.
 * @param[in] implicitValue Computed integer value.
 * @return Formatted label string (e.g. " = 10").
 */
std::string FormatEnumMemberLabel(int64_t implicitValue);

} // namespace angel_lsp::analysis
