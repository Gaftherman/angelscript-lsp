#pragma once

#include "analysis/formatting/FormattingTokens.h"
#include "analysis/formatting/FormattingTypes.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace angel_lsp::analysis::formatting
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

/**
 * @brief Places a preprocessor line on its own output line, or starts a line for a leading `[` metadata block.
 * @param[in,out] state Line builder state receiving the line.
 * @param[in] tokens Token stream.
 * @param[in,out] i Current token index; advanced past a metadata block it consumes.
 * @return True when the token was handled here.
 */
bool HandlePreprocessorOrMetadata(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i);

/**
 * @brief Handles `{`: value braces stay inline, block braces open a scope and follow the brace style.
 * @param[in,out] state Line builder state; the scope stack and brace level change.
 * @param[in,out] tokens Token stream; the brace is marked padded when it follows `)` or `function`.
 * @param[in] i Index of the `{` token.
 */
void HandleOpenBrace(LineBuilderState& state, std::vector<Token>& tokens, size_t i);

/**
 * @brief Handles `}`: closes the innermost scope and puts a block brace on its own line at the outer level.
 * @param[in,out] state Line builder state; the scope stack and brace level change.
 * @param[in,out] tokens Token stream; a value brace inherits the padding of its opener.
 * @param[in,out] i Index of the `}` token; advanced past a `;` that directly follows a block brace.
 */
void HandleCloseBrace(LineBuilderState& state, std::vector<Token>& tokens, size_t& i);

/**
 * @brief Handles `else`, `case`/`default` labels and access specifiers, in that order.
 * @param[in,out] state Line builder state.
 * @param[in] tokens Token stream.
 * @param[in,out] i Current token index; advanced past a consumed label colon.
 * @return True when one of the three handlers consumed the token.
 */
bool HandleKeywordsAndLabels(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i);

/**
 * @brief Keeps a comment that follows on the same source line attached to the current output line.
 * @param[in,out] state Line builder state receiving the comment token.
 * @param[in] tokens Token stream.
 * @param[in,out] i Current token index; advanced to the comment when one is consumed.
 */
void ConsumeTrailingComment(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i);

/**
 * @brief Keeps `{}` (and a following `;`) on one line when the options allow empty or short blocks.
 * @param[in,out] state Line builder state.
 * @param[in] tokens Token stream.
 * @param[in,out] i Index of the `{`; advanced past the consumed `}` and `;`.
 * @return True when the empty block was emitted inline.
 */
bool TryHandleEmptyOrShortBlock(LineBuilderState& state, std::vector<Token>& tokens, size_t& i);

} // namespace angel_lsp::analysis::formatting
