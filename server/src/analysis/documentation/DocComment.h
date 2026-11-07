#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Extracts and formats the doc comment preceding a declaration.
 *        Understands both `///`/`//` runs and `/\*\* ... *\/` blocks. Finding the comment is this
 * @param sourceCode Text of the document the declaration is in.
 * @param declStartLine Zero-based line the declaration begins on.
 * @return Formatted markdown, or an empty string when the declaration carries no comment.
 */
std::string ExtractDocComment(std::string_view sourceCode, uint32_t declStartLine);

/**
 * @brief Checks whether the doc comment preceding a declaration contains a specific tag.
 * @param[in] sourceCode    Text of the document the declaration is in.
 * @param[in] declStartLine Zero-based line the declaration begins on.
 * @param[in] tag           Tag substring to search for (e.g. "@listpattern").
 * @return True if preceding comments contain the specified tag, false otherwise.
 */
bool HasPrecedingDocTag(std::string_view sourceCode, uint32_t declStartLine, std::string_view tag);

/**
 * @brief Checks whether the doc comment preceding a declaration byte offset contains a specific tag.
 * @param[in] sourceCode     Text of the document the declaration is in.
 * @param[in] declStartByte  Zero-based byte offset where the declaration begins.
 * @param[in] tag            Tag substring to search for (e.g. "@listpattern").
 * @return True if preceding comments contain the specified tag, false otherwise.
 */
bool HasPrecedingDocTagAtByte(std::string_view sourceCode, uint32_t declStartByte, std::string_view tag);
} // namespace angel_lsp::analysis
