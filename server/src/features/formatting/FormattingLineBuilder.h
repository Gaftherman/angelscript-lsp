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

/**
 * @brief Indentation scope classification for source code reformatting.
 */
enum class FormattingScopeKind
{
    Generic,
    Switch,
    Enum,
    Class,
    Namespace,
    Function,
    Control,
    Value
};

/**
 * @brief Maintains formatting indentation levels and scope delimiters for nested code blocks.
 */
struct ScopeEntry
{
    FormattingScopeKind kind = FormattingScopeKind::Generic;
    int parenDepthAtOpen = 0;
    int bracketDepthAtOpen = 0;
    bool paddedBrace = false;
    bool didIncrement = true;
};

/**
 * @brief Accumulates formatted tokens, indentation prefixes, and line break directives during code reformatting.
 */
struct LineBuilderState
{
    std::vector<LineInfo> lines;
    LineInfo currentLine;
    int braceLevel = 0;
    int parenDepth = 0;
    int bracketDepth = 0;
    std::vector<ScopeEntry> scopeStack;
    FormattingScopeKind pendingScope = FormattingScopeKind::Generic;
    bool insideCaseBody = false;
    bool inValueContext = false;
    FormatCodeOptions opts;

    FormattingScopeKind CurrentScopeKind() const
    {
        return scopeStack.empty() ? FormattingScopeKind::Generic : scopeStack.back().kind;
    }

    void FlushCurrentLine()
    {
        if (!currentLine.tokenIndices.empty() || currentLine.isBlankLine)
        {
            lines.push_back(std::move(currentLine));
            currentLine = LineInfo{};
        }
    }

    void BeginLineIfEmpty()
    {
        if (!currentLine.tokenIndices.empty())
            return;
        const bool inCaseBody = insideCaseBody && CurrentScopeKind() == FormattingScopeKind::Switch;
        if (inCaseBody)
        {
            currentLine.indentLevel = opts.indentCaseLabels ? braceLevel + 1 : braceLevel;
        }
        else
        {
            currentLine.indentLevel = braceLevel;
        }
    }
};
} // namespace angel_lsp::features
