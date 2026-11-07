#include "analysis/formatting/FormattingLineBuilder.h"

#include <algorithm>

namespace angel_lsp::analysis::formatting
{

static void HandleBlankLines(LineBuilderState& state, const Token& tok)
{
    if (tok.newlinesBefore >= 2 && !state.lines.empty() && !state.lines.back().isBlankLine)
    {
        state.FlushCurrentLine();
        uint32_t maxKeep = state.opts.maxEmptyLinesToKeep;
        uint32_t blankCount = std::min(tok.newlinesBefore - 1, maxKeep);
        for (uint32_t b = 0; b < blankCount; ++b)
        {
            LineInfo blank;
            blank.isBlankLine = true;
            state.lines.push_back(std::move(blank));
        }
    }
}

static void UpdatePendingScopeFromKeyword(LineBuilderState& state, std::string_view text)
{
    static constexpr std::pair<std::string_view, FormattingScopeKind> kTable[] = {
        {"switch", FormattingScopeKind::Switch},       {"enum", FormattingScopeKind::Enum},
        {"class", FormattingScopeKind::Class},         {"interface", FormattingScopeKind::Class},
        {"namespace", FormattingScopeKind::Namespace}, {"if", FormattingScopeKind::Control},
        {"else", FormattingScopeKind::Control},        {"for", FormattingScopeKind::Control},
        {"foreach", FormattingScopeKind::Control},     {"while", FormattingScopeKind::Control},
        {"do", FormattingScopeKind::Control},          {"try", FormattingScopeKind::Control},
        {"catch", FormattingScopeKind::Control}};
    for (const auto& [kw, kind] : kTable)
    {
        if (text == kw)
        {
            state.pendingScope = kind;
            return;
        }
    }
}

static void UpdateBracketNesting(LineBuilderState& state, const Token& tok)
{
    if (tok.type == TokenType::Keyword)
    {
        UpdatePendingScopeFromKeyword(state, tok.text);
    }
    if (tok.type == TokenType::OpenParen)
    {
        state.parenDepth++;
    }
    else if (tok.type == TokenType::CloseParen && state.parenDepth > 0)
    {
        state.parenDepth--;
        if (state.parenDepth == 0 && state.pendingScope == FormattingScopeKind::Generic)
            state.pendingScope = FormattingScopeKind::Function;
    }
    else if (tok.type == TokenType::OpenBracket)
    {
        state.bracketDepth++;
    }
    else if (tok.type == TokenType::CloseBracket && state.bracketDepth > 0)
    {
        state.bracketDepth--;
    }
}

static void UpdateValueContext(LineBuilderState& state, const Token& tok)
{
    const int baseParen = state.scopeStack.empty() ? 0 : state.scopeStack.back().parenDepthAtOpen;
    const int baseBracket = state.scopeStack.empty() ? 0 : state.scopeStack.back().bracketDepthAtOpen;
    if (state.parenDepth == baseParen && state.bracketDepth == baseBracket &&
        state.CurrentScopeKind() != FormattingScopeKind::Value)
    {
        if ((tok.type == TokenType::Operator && tok.text == "=") ||
            (tok.type == TokenType::Keyword && tok.text == "return"))
        {
            state.inValueContext = true;
        }
        else if (tok.type == TokenType::Semicolon || tok.type == TokenType::Comma)
        {
            state.inValueContext = false;
        }
    }
}

static void HandleStatementOrCommentEnd(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
{
    const auto& tok = tokens[i];
    if (tok.isUnterminated || tok.type == TokenType::LineComment)
    {
        state.FlushCurrentLine();
        return;
    }
    const bool isTerm = (state.parenDepth == 0 && state.bracketDepth == 0) &&
                        ((tok.type == TokenType::Semicolon && state.CurrentScopeKind() != FormattingScopeKind::Value) ||
                         (tok.type == TokenType::Comma && state.CurrentScopeKind() == FormattingScopeKind::Enum));
    if (isTerm)
    {
        ConsumeTrailingComment(state, tokens, i);
        state.FlushCurrentLine();
        return;
    }
    if (tok.type == TokenType::Colon && IsAccessSpecifierOrLabelColon(tokens, i))
    {
        state.FlushCurrentLine();
        if (state.CurrentScopeKind() == FormattingScopeKind::Switch)
            state.insideCaseBody = true;
    }
}

std::vector<LineInfo> BuildFormattedLines(std::vector<Token>& tokens, const FormatCodeOptions& options)
{
    LineBuilderState state;
    state.opts = options;

    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const auto& tok = tokens[i];
        if (tok.isClangFormatOff)
        {
            if (tok.newlinesBefore > 0)
                state.FlushCurrentLine();
            state.BeginLineIfEmpty();
            state.currentLine.isClangFormatOff = true;
            state.currentLine.tokenIndices.push_back(i);
            continue;
        }
        if (state.currentLine.isClangFormatOff)
            state.FlushCurrentLine();

        HandleBlankLines(state, tok);
        UpdateBracketNesting(state, tok);
        UpdateValueContext(state, tok);

        if (HandlePreprocessorOrMetadata(state, tokens, i))
            continue;
        if (tok.type == TokenType::OpenBrace)
        {
            if (TryHandleEmptyOrShortBlock(state, tokens, i))
                continue;
            HandleOpenBrace(state, tokens, i);
            continue;
        }
        if (tok.type == TokenType::CloseBrace)
        {
            HandleCloseBrace(state, tokens, i);
            continue;
        }
        if (HandleKeywordsAndLabels(state, tokens, i))
            continue;

        state.BeginLineIfEmpty();
        state.currentLine.tokenIndices.push_back(i);
        HandleStatementOrCommentEnd(state, tokens, i);
    }

    state.FlushCurrentLine();
    return state.lines;
}

std::optional<MatchedLineRange> FindMatchedRange(const std::vector<LineInfo>& lines, const std::vector<Token>& tokens,
                                                 uint32_t startLine, uint32_t endLine)
{
    std::optional<size_t> firstMatchedIdx;
    std::optional<size_t> lastMatchedIdx;
    uint32_t actualStartLine = startLine;
    uint32_t actualEndLine = endLine;

    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].tokenIndices.empty())
            continue;
        uint32_t minLine = tokens[lines[i].tokenIndices.front()].line;
        uint32_t maxLine = tokens[lines[i].tokenIndices.back()].line;
        if (minLine <= endLine && maxLine >= startLine)
        {
            if (!firstMatchedIdx)
                firstMatchedIdx = i;
            lastMatchedIdx = i;
            actualStartLine = std::min(actualStartLine, minLine);
            actualEndLine = std::max(actualEndLine, maxLine);
        }
    }

    if (!firstMatchedIdx || !lastMatchedIdx)
        return std::nullopt;
    return MatchedLineRange{*firstMatchedIdx, *lastMatchedIdx, actualStartLine, actualEndLine};
}
} // namespace angel_lsp::analysis::formatting
