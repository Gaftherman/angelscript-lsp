#pragma once

#include "features/hover/HoverHandler.h"

#include <optional>
#include <string>
#include <string_view>

namespace angel_lsp::features
{
/**
 * @brief Formats primitive type documentation including localized description, values or range.
 * @param[in] typeName Primitive type name.
 * @param[in] i18n Optional localization dictionary pointer.
 * @return Formatted markdown string.
 */
std::string FormatPrimitiveDoc(std::string_view typeName, const angel_lsp::i18n::I18n* i18n);

/**
 * @brief Attempts to produce hover documentation for primitive types.
 * @param[in] nodeText Text of the hover target.
 * @param[in] range Target range in the document.
 * @param[in] i18n Optional localization dictionary pointer.
 * @param[in,out] profiler Performance profiler.
 * @return Populated Hover if nodeText is a primitive type, std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverPrimitiveType(std::string_view nodeText, const lsp::Range& range,
                                                const angel_lsp::i18n::I18n* i18n, HoverProfiler& profiler);

/**
 * @brief Attempts to produce hover documentation for the wildcard type '?'.
 * @param[in] nodeText Text of the hover target.
 * @param[in] range Target range in the document.
 * @param[in] i18n Optional localization dictionary pointer.
 * @param[in,out] profiler Performance profiler.
 * @return Populated Hover if nodeText is '?', std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverWildcardType(std::string_view nodeText, const lsp::Range& range,
                                               const angel_lsp::i18n::I18n* i18n, HoverProfiler& profiler);

/**
 * @brief Attempts to produce hover documentation for 'this' pointer in class/mixin context.
 * @param[in] ctx Hover query context.
 * @return Populated Hover if node is 'this', std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverThis(const HoverQueryContext& ctx);

/**
 * @brief Attempts to construct hover information when hovering on an anonymous function / lambda header.
 * @param[in] ctx Hover query context.
 * @return Optional hover tooltip if hovering on a lambda keyword or header.
 */
std::optional<lsp::Hover> TryHoverLambda(const HoverQueryContext& ctx);

/**
 * @brief Attempts to produce hover documentation for 'auto' keyword showing the deduced type.
 * @param[in] ctx Hover query context.
 * @return Populated Hover if node is 'auto' in an auto declaration, std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverAutoType(const HoverQueryContext& ctx);

/**
 * @brief Attempts to produce hover for string/binary literals or primitive/wildcard types.
 * @param[in] target Extracted hover target.
 * @param[in] request Original hover request.
 * @param[in,out] profiler Performance profiler.
 * @return Populated Hover if matched, std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverLiteralOrType(const HoverTarget& target, const HoverRequest& request,
                                                HoverProfiler& profiler);

/**
 * @brief Deduces type of an auto variable declarator AST node.
 * @param[in] declarator Variable declarator TSNode.
 * @param[in] ctx Hover query context.
 * @return Inferred type name or empty string.
 */
std::string DeduceAutoDeclaratorType(TSNode declarator, const HoverQueryContext& ctx);

/**
 * @brief Deduces hover type for an in-scope local definition.
 * @param[in] def Local definition descriptor.
 * @param[in] ctx Hover query context.
 * @return Inferred type name or original definition type.
 */
std::string DeduceLocalDefinitionHoverType(const analysis::LocalDefinition& def, const HoverQueryContext& ctx);

/**
 * @brief Infers type string from enclosing parameter or variable declaration AST node.
 * @param[in] node Target AST node.
 * @param[in] sourceCode Source text buffer.
 * @return Extracted type string or empty.
 */
std::string InferTypeFromAst(TSNode node, const std::string& sourceCode);

/**
 * @brief Formats raw byte count into human-readable B, KB, or MB units.
 * @param[in] bytes File size in bytes.
 * @return Formatted size string.
 */
std::string FormatFileSize(uintmax_t bytes);

/**
 * @brief Resolves a string literal to an absolute asset file path if one exists.
 * @param[in] unquoted Unquoted string literal text.
 * @param[in] request Hover request context.
 * @return Normalized absolute path if file exists, or empty string.
 */
std::string ResolveStringLiteralPath(std::string_view unquoted, const HoverRequest& request);

/**
 * @brief Attempts to hover a string literal with optional asset resolution and length.
 * @param[in] target Hover target info.
 * @param[in] request Hover request context.
 * @return Hover result if string literal, std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverStringLiteral(const HoverTarget& target, const HoverRequest& request);

/**
 * @brief Attempts to hover a concatenated string expression if it resolves to a valid asset file.
 * @param[in] target Hover target info.
 * @param[in] request Hover request context.
 * @param[in] showLength Whether to display character length.
 * @return Hover result if asset resolved, std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverConcatenatedString(const HoverTarget& target, const HoverRequest& request,
                                                     bool showLength);
} // namespace angel_lsp::features
