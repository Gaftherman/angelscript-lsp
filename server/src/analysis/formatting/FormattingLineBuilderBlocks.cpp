#include "analysis/formatting/FormattingLineBuilder.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::analysis::formatting
{

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

static bool ShouldAttachCustomBrace(FormattingScopeKind kind, const BraceWrappingOptions& wrap)
{
    if (kind == FormattingScopeKind::Class)
        return !wrap.afterClass;
    if (kind == FormattingScopeKind::Enum)
        return !wrap.afterEnum;
    if (kind == FormattingScopeKind::Function)
        return !wrap.afterFunction;
    if (kind == FormattingScopeKind::Namespace)
        return !wrap.afterNamespace;
    return !wrap.afterControlStatement;
}

static bool ShouldAttachOpenBrace(FormattingScopeKind kind, const FormatCodeOptions& opts)
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
        return kind != FormattingScopeKind::Function && kind != FormattingScopeKind::Class &&
               kind != FormattingScopeKind::Namespace;
    case BraceStyle::Mozilla:
        return kind != FormattingScopeKind::Function && kind != FormattingScopeKind::Class &&
               kind != FormattingScopeKind::Enum;
    case BraceStyle::Custom:
        return ShouldAttachCustomBrace(kind, opts.braceWrapping);
    }
    return false;
}

void HandleOpenBrace(LineBuilderState& state, std::vector<Token>& tokens, size_t i)
{
    const int baseParen = state.scopeStack.empty() ? 0 : state.scopeStack.back().parenDepthAtOpen;
    const int baseBracket = state.scopeStack.empty() ? 0 : state.scopeStack.back().bracketDepthAtOpen;
    const bool isValueBrace = state.CurrentScopeKind() == FormattingScopeKind::Value || state.parenDepth > baseParen ||
                              state.bracketDepth > baseBracket || state.inValueContext;

    if (isValueBrace)
    {
        const bool padded = i > 0 && (tokens[i - 1].type == TokenType::CloseParen || tokens[i - 1].text == "function");
        tokens[i].isPaddedBrace = padded;
        state.BeginLineIfEmpty();
        state.currentLine.tokenIndices.push_back(i);
        state.scopeStack.push_back({FormattingScopeKind::Value, state.parenDepth, state.bracketDepth, padded, false});
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

    bool doInc = !(state.pendingScope == FormattingScopeKind::Namespace &&
                   state.opts.namespaceIndentation == NamespaceIndentationStyle::None);
    state.scopeStack.push_back({state.pendingScope, state.parenDepth, state.bracketDepth, false, doInc});
    state.pendingScope = FormattingScopeKind::Generic;
    state.insideCaseBody = false;
    state.inValueContext = false;
    if (doInc)
        state.braceLevel++;
}

void HandleCloseBrace(LineBuilderState& state, std::vector<Token>& tokens, size_t& i)
{
    if (state.CurrentScopeKind() == FormattingScopeKind::Value)
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

static bool ShouldAttachElse(const FormatCodeOptions& opts)
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

static bool HandleElseKeyword(LineBuilderState& state, const std::vector<Token>& tokens, size_t i)
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

static bool HandleCaseOrDefault(LineBuilderState& state, const Token& tok, size_t i)
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

static bool HandleAccessSpecifier(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
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
        state.pendingScope = FormattingScopeKind::Generic;
        state.insideCaseBody = false;
        state.inValueContext = false;
        return true;
    }
    return false;
}
} // namespace angel_lsp::analysis::formatting
