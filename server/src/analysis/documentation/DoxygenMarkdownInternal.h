#pragma once

#include "analysis/documentation/DoxygenMarkdown.h"
#include "parser/DoxygenSyntax.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::doxygen
{
// --- Defined in DoxygenText.cpp ---

std::string Trim(std::string_view str);

/**
 * @brief Strips leading ASCII whitespace.
 * @param[in] str Source text to trim.
 * @return Sub-string with leading whitespace removed.
 */
std::string TrimLeading(std::string_view str);

/**
 * @brief Strips trailing ASCII whitespace in place.
 * @param[in,out] str String buffer modified in place.
 */
void TrimTrailing(std::string& str);

/**
 * @brief Converts ASCII string to lowercase.
 * @param[in] str Source string to convert.
 * @return Transformed lowercase string.
 */
std::string ToLower(std::string_view str);

/**
 * @brief Returns true if string contains at least one ASCII alphanumeric character.
 * @param[in] text String view to inspect.
 * @return True if alphanumeric characters are present; false otherwise.
 */
bool HasAlphanumeric(std::string_view text);

/**
 * @brief Strips trailing stray semicolons from stub comments outside verbatim blocks.
 * @param[in] line Comment line text to process.
 * @return Processed line with trailing semicolon removed if applicable.
 */
std::string StripTrailingStubSemicolon(std::string_view line);

/**
 * @brief Splits text into lines, stripping trailing carriage returns.
 * @param[in] str Multiline text buffer.
 * @return Vector of split line strings.
 */
std::vector<std::string> SplitLines(std::string_view str);

/**
 * @brief Strips leading comment star decoration [ \t]*\*[ \t]? from a line.
 *
 * Required for lines after the first in multi-line descriptions and code blocks.
 */
std::string StripLineLeadingStar(std::string_view line);

/**
 * @brief Determines whether a newline should be preserved between two consecutive description lines.
 * @param[in] prev Preceding line content.
 * @param[in] curr Following line content.
 * @return True if line break must be retained; false if lines can flow together.
 */
bool ShouldKeepDescriptionNewline(std::string_view prev, std::string_view curr);

/**
 * @brief Cleans description lines by stripping line decorations, converting `-#` list items,
 * and joining lines with single spaces or linebreaks (for lists and punctuated clauses).
 */
std::string CleanDescriptionLines(std::string_view raw);

/**
 * @brief Checks if trailing character is punctuation that should remain outside formatting.
 * @param[in] c Character under test.
 * @return True if character is trailing punctuation; false otherwise.
 */
bool IsTrailingPunctuation(char c);

/**
 * @brief Rewrites at-form inline formatting commands (@c, @p, @b, @a, @e, @em, @ref) in verbatim text.
 */
std::string RewriteAtInlineCommands(std::string_view input);

/**
 * @brief Checks if a token is a known Doxygen command starting with 'n'.
 */
bool IsKnownDoxygenCommandStartingWithN(std::string_view text);

// --- Defined in DoxygenCommentLines.cpp ---

/**
 * @brief Extracts the content lines of a raw comment, whichever way it was written.
 */
std::vector<std::string> ExtractCommentLines(std::string_view rawComment);

/**
 * @brief Extracts the brief description in the pre-pass and removes it from lines.
 */
std::string ExtractBrief(std::vector<std::string>& lines);

/**
 * @brief Parses language identifier from code block command arguments.
 * @param[in] afterCmd Slice following the code command keyword.
 * @return Parsed code language token, e.g. angelscript or cpp.
 */
std::string ParseCodeLanguage(std::string_view afterCmd);

/**
 * @brief Segments comment lines into Tag, Text, and VerbatimCode blocks.
 */
std::vector<CommentSegment> SegmentCommentLines(const std::vector<std::string>& lines);

/**
 * @brief Wraps lines into a synthetic block comment for parsing.
 * @param[in] lines Collection of comment lines.
 * @return Synthetic C-style comment block string suitable for AST parsing.
 */
std::string WrapInSyntheticComment(const std::vector<std::string>& lines);

// --- Defined in DoxygenTagRender.cpp ---

/**
 * @brief Parses a single synthetic document segment and collects AST DocBlocks.
 */
void ParseDocSegment(std::string_view syntheticDoc, std::vector<DocBlock>& blocks);
} // namespace angel_lsp::analysis::doxygen
