#pragma once

#include "features/formatting/FormattingTokens.h"
#include "features/formatting/FormattingTypes.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Representation of a formatted line with its indentation and token sequence.
 */
struct LineInfo
{
    int indentLevel = 0;
    bool isPreprocessor = false;
    bool isBlankLine = false;
    bool isClangFormatOff = false;
    std::vector<size_t> tokenIndices;
};

/**
 * @brief Line range corresponding to a requested formatting slice.
 */
struct MatchedLineRange
{
    size_t firstIdx = 0;
    size_t lastIdx = 0;
    uint32_t actualStartLine = 0;
    uint32_t actualEndLine = 0;
};

/**
 * @brief Organizes tokens into formatted line structures according to style options.
 * @param[in,out] tokens Annotated token stream.
 * @param[in] options Effective formatting configuration options.
 * @return Vector of formatted line specifications.
 */
std::vector<LineInfo> BuildFormattedLines(std::vector<Token>& tokens, const FormatCodeOptions& options);

/**
 * @brief Locates the sub-range of formatted lines overlapping [startLine, endLine].
 * @param[in] lines Formatted line specifications.
 * @param[in] tokens Annotated token stream.
 * @param[in] startLine 0-indexed start line.
 * @param[in] endLine 0-indexed end line.
 * @return Matched range if overlapping tokens exist, std::nullopt otherwise.
 */
std::optional<MatchedLineRange> FindMatchedRange(const std::vector<LineInfo>& lines, const std::vector<Token>& tokens,
                                                 uint32_t startLine, uint32_t endLine);
} // namespace angel_lsp::features
