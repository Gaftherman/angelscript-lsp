#include "features/formatting/FormattingLineBuilder.h"
#include <algorithm>

namespace angel_lsp::features
{
namespace
{
enum class ScopeKind
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

struct ScopeEntry
{
    ScopeKind kind = ScopeKind::Generic;
    int parenDepthAtOpen = 0;
    int bracketDepthAtOpen = 0;
    bool paddedBrace = false;
    bool didIncrement = true;
};

struct LineBuilderState
{
    std::vector<LineInfo> lines;
    LineInfo currentLine;
    int braceLevel = 0;
    int parenDepth = 0;
    int bracketDepth = 0;
    std::vector<ScopeEntry> scopeStack;
    ScopeKind pendingScope = ScopeKind::Generic;
    bool insideCaseBody = false;
    bool inValueContext = false;
    FormatCodeOptions opts;

    ScopeKind CurrentScopeKind() const
    {
        return scopeStack.empty() ? ScopeKind::Generic : scopeStack.back().kind;
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
        const bool inCaseBody = insideCaseBody && CurrentScopeKind() == ScopeKind::Switch;
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

void HandleBlankLines(LineBuilderState& state, const Token& tok)
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

void UpdatePendingScopeFromKeyword(LineBuilderState& state, std::string_view text)
{
    static constexpr std::pair<std::string_view, ScopeKind> kTable[] = {
        {"switch", ScopeKind::Switch},   {"enum", ScopeKind::Enum},           {"class", ScopeKind::Class},
        {"interface", ScopeKind::Class}, {"namespace", ScopeKind::Namespace}, {"if", ScopeKind::Control},
        {"else", ScopeKind::Control},    {"for", ScopeKind::Control},         {"foreach", ScopeKind::Control},
        {"while", ScopeKind::Control},   {"do", ScopeKind::Control},          {"try", ScopeKind::Control},
        {"catch", ScopeKind::Control}};
    for (const auto& [kw, kind] : kTable)
    {
        if (text == kw)
        {
            state.pendingScope = kind;
            return;
        }
    }
}

void UpdateBracketNesting(LineBuilderState& state, const Token& tok)
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
        if (state.parenDepth == 0 && state.pendingScope == ScopeKind::Generic)
            state.pendingScope = ScopeKind::Function;
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

void UpdateValueContext(LineBuilderState& state, const Token& tok)
{
    const int baseParen = state.scopeStack.empty() ? 0 : state.scopeStack.back().parenDepthAtOpen;
    const int baseBracket = state.scopeStack.empty() ? 0 : state.scopeStack.back().bracketDepthAtOpen;
    if (state.parenDepth == baseParen && state.bracketDepth == baseBracket &&
        state.CurrentScopeKind() != ScopeKind::Value)
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

bool HandlePreprocessorOrMetadata(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
{
    const auto& tok = tokens[i];
    if (tok.type == TokenType::Preprocessor)
    {
        state.FlushCurrentLine();
        LineInfo prep;
        prep.isPreprocessor = true;
        prep.indentLevel = state.braceLevel;
        prep.tokenIndices.push_back(i);
        state.lines.push_back(std::move(prep));
        return true;
    }
    if (tok.type == TokenType::OpenBracket && state.currentLine.tokenIndices.empty())
    {
        state.BeginLineIfEmpty();
        state.currentLine.tokenIndices.push_back(i);
        int depth = 1;
        while (depth > 0 && i + 1 < tokens.size())
        {
            ++i;
            if (tokens[i].type == TokenType::OpenBracket)
            {
                depth++;
                state.bracketDepth++;
            }
            else if (tokens[i].type == TokenType::CloseBracket)
            {
                depth--;
                if (state.bracketDepth > 0)
                    state.bracketDepth--;
            }
            state.currentLine.tokenIndices.push_back(i);
        }
        state.FlushCurrentLine();
        return true;
    }
    return false;
}

bool ShouldAttachCustomBrace(ScopeKind kind, const BraceWrappingOptions& wrap)
{
    if (kind == ScopeKind::Class)
        return !wrap.afterClass;
    if (kind == ScopeKind::Enum)
        return !wrap.afterEnum;
    if (kind == ScopeKind::Function)
        return !wrap.afterFunction;
    if (kind == ScopeKind::Namespace)
        return !wrap.afterNamespace;
    return !wrap.afterControlStatement;
}

bool ShouldAttachOpenBrace(ScopeKind kind, const FormatCodeOptions& opts)
{
    switch (opts.braceStyle)
    {
    case BraceStyle::Allman:
    case BraceStyle::GNU:
    case BraceStyle::Whitesmiths:
        return false;
    case BraceStyle::KAndR:
    case BraceStyle::Stroustrup:
        return true;
    case BraceStyle::Linux:
    case BraceStyle::WebKit:
        return kind != ScopeKind::Function && kind != ScopeKind::Class && kind != ScopeKind::Namespace;
    case BraceStyle::Mozilla:
        return kind != ScopeKind::Function && kind != ScopeKind::Class && kind != ScopeKind::Enum;
    case BraceStyle::Custom:
        return ShouldAttachCustomBrace(kind, opts.braceWrapping);
    }
    return false;
}

void HandleOpenBrace(LineBuilderState& state, std::vector<Token>& tokens, size_t i)
{
    const int baseParen = state.scopeStack.empty() ? 0 : state.scopeStack.back().parenDepthAtOpen;
    const int baseBracket = state.scopeStack.empty() ? 0 : state.scopeStack.back().bracketDepthAtOpen;
    const bool isValueBrace = state.CurrentScopeKind() == ScopeKind::Value || state.parenDepth > baseParen ||
                              state.bracketDepth > baseBracket || state.inValueContext;

    if (isValueBrace)
    {
        const bool padded = i > 0 && (tokens[i - 1].type == TokenType::CloseParen || tokens[i - 1].text == "function");
        tokens[i].isPaddedBrace = padded;
        state.BeginLineIfEmpty();
        state.currentLine.tokenIndices.push_back(i);
        state.scopeStack.push_back({ScopeKind::Value, state.parenDepth, state.bracketDepth, padded, false});
        return;
    }

    if (ShouldAttachOpenBrace(state.pendingScope, state.opts) && !state.currentLine.tokenIndices.empty())
    {
        state.currentLine.tokenIndices.push_back(i);
        state.FlushCurrentLine();
    }
    else
    {
        state.FlushCurrentLine();
        LineInfo braceLine;
        braceLine.indentLevel =
            (state.opts.braceStyle == BraceStyle::Whitesmiths) ? state.braceLevel + 1 : state.braceLevel;
        braceLine.tokenIndices.push_back(i);
        state.lines.push_back(std::move(braceLine));
    }

    bool doInc = !(state.pendingScope == ScopeKind::Namespace &&
                   state.opts.namespaceIndentation == NamespaceIndentationStyle::None);
    state.scopeStack.push_back({state.pendingScope, state.parenDepth, state.bracketDepth, false, doInc});
    state.pendingScope = ScopeKind::Generic;
    state.insideCaseBody = false;
    state.inValueContext = false;
    if (doInc)
        state.braceLevel++;
}

void HandleCloseBrace(LineBuilderState& state, std::vector<Token>& tokens, size_t& i)
{
    if (state.CurrentScopeKind() == ScopeKind::Value)
    {
        tokens[i].isPaddedBrace = state.scopeStack.back().paddedBrace;
        state.scopeStack.pop_back();
        state.BeginLineIfEmpty();
        state.currentLine.tokenIndices.push_back(i);
        return;
    }

    state.FlushCurrentLine();
    bool didInc = state.scopeStack.empty() ? true : state.scopeStack.back().didIncrement;
    if (didInc)
        state.braceLevel = std::max(0, state.braceLevel - 1);
    if (!state.scopeStack.empty())
        state.scopeStack.pop_back();
    state.insideCaseBody = false;
    state.inValueContext = false;

    LineInfo braceLine;
    braceLine.indentLevel =
        (state.opts.braceStyle == BraceStyle::Whitesmiths) ? state.braceLevel + 1 : state.braceLevel;
    braceLine.tokenIndices.push_back(i);

    if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Semicolon)
    {
        i++;
        braceLine.tokenIndices.push_back(i);
    }
    state.lines.push_back(std::move(braceLine));
}

bool ShouldAttachElse(const FormatCodeOptions& opts)
{
    if (opts.braceStyle == BraceStyle::KAndR || opts.braceStyle == BraceStyle::Linux ||
        opts.braceStyle == BraceStyle::Mozilla || opts.braceStyle == BraceStyle::WebKit)
    {
        return true;
    }
    if (opts.braceStyle == BraceStyle::Custom)
        return !opts.braceWrapping.beforeElse;
    return false;
}

bool HandleElseKeyword(LineBuilderState& state, const std::vector<Token>& tokens, size_t i)
{
    if (tokens[i].type != TokenType::Keyword || tokens[i].text != "else")
        return false;
    state.FlushCurrentLine();
    if (ShouldAttachElse(state.opts) && !state.lines.empty() && state.lines.back().tokenIndices.size() == 1 &&
        tokens[state.lines.back().tokenIndices.front()].type == TokenType::CloseBrace)
    {
        state.currentLine = std::move(state.lines.back());
        state.lines.pop_back();
        state.currentLine.tokenIndices.push_back(i);
        return true;
    }
    state.currentLine.indentLevel = state.braceLevel;
    state.currentLine.tokenIndices.push_back(i);
    return true;
}

bool HandleCaseOrDefault(LineBuilderState& state, const Token& tok, size_t i)
{
    if (tok.type == TokenType::Keyword && (tok.text == "case" || tok.text == "default"))
    {
        state.FlushCurrentLine();
        state.insideCaseBody = false;
        state.currentLine.indentLevel =
            state.opts.indentCaseLabels ? state.braceLevel : std::max(0, state.braceLevel - 1);
        state.currentLine.tokenIndices.push_back(i);
        return true;
    }
    return false;
}

bool HandleAccessSpecifier(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
{
    const auto& tok = tokens[i];
    if (tok.type == TokenType::Keyword && (tok.text == "public" || tok.text == "private" || tok.text == "protected") &&
        i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Colon)
    {
        state.FlushCurrentLine();
        state.currentLine.indentLevel =
            (state.opts.accessModifierOffset < 0) ? std::max(0, state.braceLevel - 1) : state.braceLevel;
        state.currentLine.tokenIndices.push_back(i);
        i++;
        state.currentLine.tokenIndices.push_back(i);
        state.FlushCurrentLine();
        return true;
    }
    return false;
}

bool HandleKeywordsAndLabels(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
{
    return HandleElseKeyword(state, tokens, i) || HandleCaseOrDefault(state, tokens[i], i) ||
           HandleAccessSpecifier(state, tokens, i);
}

void ConsumeTrailingComment(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
{
    if (i + 1 < tokens.size() && tokens[i + 1].newlinesBefore == 0 &&
        (tokens[i + 1].type == TokenType::LineComment || tokens[i + 1].type == TokenType::BlockComment))
    {
        i++;
        state.currentLine.tokenIndices.push_back(i);
    }
}

void HandleStatementOrCommentEnd(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
{
    const auto& tok = tokens[i];
    if (tok.isUnterminated || tok.type == TokenType::LineComment)
    {
        state.FlushCurrentLine();
        return;
    }
    const bool isTerm = (state.parenDepth == 0 && state.bracketDepth == 0) &&
                        ((tok.type == TokenType::Semicolon && state.CurrentScopeKind() != ScopeKind::Value) ||
                         (tok.type == TokenType::Comma && state.CurrentScopeKind() == ScopeKind::Enum));
    if (isTerm)
    {
        ConsumeTrailingComment(state, tokens, i);
        state.FlushCurrentLine();
        return;
    }
    if (tok.type == TokenType::Colon && IsAccessSpecifierOrLabelColon(tokens, i))
    {
        state.FlushCurrentLine();
        if (state.CurrentScopeKind() == ScopeKind::Switch)
            state.insideCaseBody = true;
    }
}

bool TryHandleEmptyOrShortBlock(LineBuilderState& state, std::vector<Token>& tokens, size_t& i)
{
    const bool allowEmpty =
        state.opts.keepEmptyBlocksOnSingleLine || state.opts.allowShortBlocksOnASingleLine != ShortBlockStyle::Never;
    if (allowEmpty && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::CloseBrace &&
        tokens[i + 1].newlinesBefore == 0)
    {
        if (tokens[i].newlinesBefore > 0)
            state.FlushCurrentLine();
        state.BeginLineIfEmpty();
        state.currentLine.tokenIndices.push_back(i);
        state.currentLine.tokenIndices.push_back(i + 1);
        i++;
        if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Semicolon && tokens[i + 1].newlinesBefore == 0)
        {
            i++;
            state.currentLine.tokenIndices.push_back(i);
        }
        ConsumeTrailingComment(state, tokens, i);
        state.FlushCurrentLine();
        state.pendingScope = ScopeKind::Generic;
        state.insideCaseBody = false;
        state.inValueContext = false;
        return true;
    }
    return false;
}
} // namespace

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
} // namespace angel_lsp::features
