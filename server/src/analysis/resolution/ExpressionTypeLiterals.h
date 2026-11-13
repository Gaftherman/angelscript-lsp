#pragma once

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Parses numeric literal text to determine primitive type.
 * @param[in] text Numeric literal spelling.
 * @return Primitive type name (float, double, uint64, int64, uint, int).
 */
std::string_view ResolveNumberLiteral(std::string_view text);

/**
 * @brief Identifies whether literal text represents floating point.
 * @param[in] text Literal spelling.
 * @return "float", "double", or empty string.
 */
std::string_view ResolveFloatingPointLiteral(std::string_view text);

/**
 * @brief Determines integer type from suffix letters.
 * @param[in] text Integer literal spelling.
 * @return Suffix type name.
 */
std::string_view ResolveIntegerLiteralSuffix(std::string_view text);

} // namespace angel_lsp::analysis
