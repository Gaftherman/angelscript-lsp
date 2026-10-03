#include "features/formatting/FormattingSpacing.h"
#include <optional>
#include <unordered_set>

namespace angel_lsp::features
{
namespace
{
static const std::unordered_set<std::string_view> kControlKeywords = {"if",    "for",    "foreach",
                                                                      "while", "switch", "catch"};

bool IsClosingOrSeparatorPunctuation(TokenType type)
{
    switch (type)
    {
    case TokenType::Comma:
    case TokenType::Semicolon:
    case TokenType::CloseParen:
    case TokenType::CloseBracket:
    case TokenType::Dot:
    case TokenType::DoubleColon:
        return true;
    default:
        return false;
    }
}

bool IsOpeningOrMemberPunctuation(TokenType type)
{
    switch (type)
    {
    case TokenType::OpenParen:
    case TokenType::OpenBracket:
    case TokenType::Dot:
    case TokenType::DoubleColon:
        return true;
    default:
        return false;
    }
}

std::optional<bool> CheckPunctuationSpacing(const Token& prev, const Token& curr, const FormatCodeOptions& opts)
{
    if (curr.type == TokenType::Comma && (prev.type == TokenType::Comma || prev.type == TokenType::OpenBrace))
        return true;
    if (opts.spacesInsideParentheses)
    {
        if (prev.type == TokenType::OpenParen)
            return curr.type != TokenType::CloseParen;
        if (curr.type == TokenType::CloseParen)
            return prev.type != TokenType::OpenParen;
    }
    if (opts.spacesInSquareBrackets)
    {
        if (prev.type == TokenType::OpenBracket)
            return curr.type != TokenType::CloseBracket;
        if (curr.type == TokenType::CloseBracket)
            return prev.type != TokenType::OpenBracket;
    }
    if (IsClosingOrSeparatorPunctuation(curr.type) || IsOpeningOrMemberPunctuation(prev.type))
        return false;
    if (prev.type == TokenType::OpenBrace)
        return prev.isPaddedBrace;
    if (curr.type == TokenType::CloseBrace)
        return curr.isPaddedBrace;
    return std::nullopt;
}

std::optional<bool> CheckUnaryAndIncrementSpacing(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx)
{
    const auto& prev = tokens[prevIdx];
    const auto& curr = tokens[currIdx];

    if ((curr.type == TokenType::Increment || curr.type == TokenType::Decrement) &&
        (prev.type == TokenType::Identifier || prev.type == TokenType::CloseBracket ||
         prev.type == TokenType::CloseParen))
    {
        return false;
    }
    if (prev.type == TokenType::Increment || prev.type == TokenType::Decrement || prev.text == "!" || prev.text == "~")
        return false;
    if (IsUnary(tokens, prevIdx))
        return false;
    if (IsUnary(tokens, currIdx))
        return !(prev.type == TokenType::OpenParen || prev.type == TokenType::OpenBracket);
    return std::nullopt;
}

PointerAlignment EffectiveRefAlignment(const FormatCodeOptions& opts)
{
    if (opts.referenceAlignment == ReferenceAlignment::Left)
        return PointerAlignment::Left;
    if (opts.referenceAlignment == ReferenceAlignment::Right)
        return PointerAlignment::Right;
    if (opts.referenceAlignment == ReferenceAlignment::Middle)
        return PointerAlignment::Middle;
    return opts.pointerAlignment;
}

std::optional<bool> CheckRightAtSpacing(const Token& prev, PointerAlignment pointerAlignment)
{
    if (prev.type == TokenType::At)
        return false;
    if (IsTypeLikePrecedingToken(prev))
        return pointerAlignment == PointerAlignment::Right || pointerAlignment == PointerAlignment::Middle;
    if (prev.type == TokenType::OpenParen || prev.type == TokenType::OpenBracket || prev.type == TokenType::OpenBrace)
        return false;
    return std::nullopt;
}

std::optional<bool> CheckLeftAtSpacing(const std::vector<Token>& tokens, size_t prevIdx, const Token& curr,
                                       const FormatCodeOptions& opts)
{
    if (curr.type == TokenType::At || curr.isTemplateCloser || curr.type == TokenType::OpenParen ||
        curr.type == TokenType::OpenBrace)
    {
        return false;
    }
    if (curr.text == "&")
    {
        return opts.referenceAlignment == ReferenceAlignment::Right ||
               opts.referenceAlignment == ReferenceAlignment::Middle;
    }
    if (curr.type == TokenType::Identifier || curr.type == TokenType::Keyword)
    {
        if (IsHandleTypeDeclarator(tokens, prevIdx))
            return opts.pointerAlignment != PointerAlignment::Right;
        return false;
    }
    return std::nullopt;
}

std::optional<bool> CheckReferenceSpacing(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx,
                                          const FormatCodeOptions& opts)
{
    const auto& prev = tokens[prevIdx];
    const auto& curr = tokens[currIdx];
    const PointerAlignment refAlign = EffectiveRefAlignment(opts);

    if (curr.text == "&" && IsReferenceTypeDeclarator(tokens, currIdx))
    {
        if (prev.type == TokenType::At)
        {
            return opts.referenceAlignment == ReferenceAlignment::Right ||
                   opts.referenceAlignment == ReferenceAlignment::Middle;
        }
        return refAlign == PointerAlignment::Right || refAlign == PointerAlignment::Middle;
    }
    if (prev.text == "&" && IsReferenceTypeDeclarator(tokens, prevIdx))
    {
        if (curr.type == TokenType::Identifier || curr.type == TokenType::Keyword)
            return refAlign != PointerAlignment::Right;
    }
    return std::nullopt;
}

std::optional<bool> CheckTemplateBracketSpacing(const Token& prev, const Token& curr, bool spacesInAngles)
{
    if (curr.isTemplateOpener)
        return false;
    if (prev.isTemplateOpener)
        return curr.isTemplateCloser ? false : spacesInAngles;
    if (curr.isTemplateCloser)
        return spacesInAngles;
    if (prev.isTemplateCloser)
    {
        if (curr.type == TokenType::OpenParen)
            return false;
        return curr.type == TokenType::Identifier || curr.type == TokenType::Keyword ||
               curr.type == TokenType::Operator;
    }
    return std::nullopt;
}

std::optional<bool> CheckAtRefAndTemplateSpacing(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx,
                                                 const FormatCodeOptions& opts)
{
    const auto& prev = tokens[prevIdx];
    const auto& curr = tokens[currIdx];

    if (curr.type == TokenType::At)
        return CheckRightAtSpacing(prev, opts.pointerAlignment);
    if (prev.type == TokenType::At)
        return CheckLeftAtSpacing(tokens, prevIdx, curr, opts);
    if (auto refRes = CheckReferenceSpacing(tokens, prevIdx, currIdx, opts))
        return refRes;
    return CheckTemplateBracketSpacing(prev, curr, opts.spacesInAngles);
}

bool IsCallLikePrecedingToken(const Token& tok)
{
    return tok.type == TokenType::Identifier || tok.text == "super" || tok.text == "this" || tok.text == "cast" ||
           tok.text == "function";
}

std::optional<bool> CheckParenAndBracketSpacing(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx,
                                                const FormatCodeOptions& opts)
{
    const auto& prev = tokens[prevIdx];
    const auto& curr = tokens[currIdx];

    if (curr.type == TokenType::OpenParen)
    {
        const bool isControl = (prev.type == TokenType::Keyword && kControlKeywords.contains(prev.text));
        const bool isCall = IsCallLikePrecedingToken(prev);
        if (isControl || isCall)
        {
            if (opts.spaceBeforeParens == SpaceBeforeParensStyle::Never)
                return false;
            if (opts.spaceBeforeParens == SpaceBeforeParensStyle::Always)
                return true;
            if (opts.spaceBeforeParens == SpaceBeforeParensStyle::NonEmptyParentheses)
                return currIdx + 1 < tokens.size() && tokens[currIdx + 1].type != TokenType::CloseParen;
            return isControl;
        }
    }
    if (curr.type == TokenType::OpenBracket)
    {
        if (prev.type == TokenType::Identifier || prev.type == TokenType::CloseBracket ||
            prev.type == TokenType::CloseParen)
        {
            return false;
        }
    }
    return std::nullopt;
}

bool IsAssignmentOperator(std::string_view op)
{
    static const std::unordered_set<std::string_view> kAssignOps = {
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>="};
    return kAssignOps.contains(op);
}

bool IsStandardBinaryOperator(const Token& tok)
{
    return tok.type == TokenType::Operator && !tok.isTemplateOpener && !tok.isTemplateCloser;
}

std::optional<bool> CheckBinaryAndColonSpacing(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx,
                                               const FormatCodeOptions& opts)
{
    const auto& prev = tokens[prevIdx];
    const auto& curr = tokens[currIdx];

    if (curr.type == TokenType::Colon)
        return !IsAccessSpecifierOrLabelColon(tokens, currIdx);
    if (prev.type == TokenType::Comma || prev.type == TokenType::Semicolon)
        return true;
    if (!opts.spaceBeforeAssignmentOperators && curr.type == TokenType::Operator && IsAssignmentOperator(curr.text))
        return false;
    if (IsStandardBinaryOperator(curr) || IsStandardBinaryOperator(prev))
        return true;
    if (curr.type == TokenType::Question || prev.type == TokenType::Question || prev.type == TokenType::Colon)
        return true;
    return std::nullopt;
}

bool IsWordOrLiteralType(TokenType type)
{
    return type == TokenType::Identifier || type == TokenType::Number || type == TokenType::StringLiteral;
}

bool IsWordOrKeywordType(TokenType type)
{
    return type == TokenType::Identifier || type == TokenType::Keyword;
}

bool CheckWordAndLiteralSpacing(const Token& prev, const Token& curr)
{
    if (prev.type == TokenType::Keyword || curr.type == TokenType::Keyword)
        return true;
    if (prev.type == TokenType::Identifier && IsWordOrLiteralType(curr.type))
        return true;
    if (prev.type == TokenType::CloseParen && (IsWordOrKeywordType(curr.type) || curr.type == TokenType::OpenBrace))
        return true;
    if (prev.type == TokenType::CloseBracket && IsWordOrKeywordType(curr.type))
        return true;
    if (curr.type == TokenType::LineComment || curr.type == TokenType::BlockComment)
        return true;
    return (prev.type == TokenType::Number || prev.type == TokenType::StringLiteral) && IsWordOrLiteralType(curr.type);
}
} // namespace

bool NeedsSpaceBetween(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx,
                       const FormatCodeOptions& options)
{
    const auto& prev = tokens[prevIdx];
    const auto& curr = tokens[currIdx];

    if (auto res = CheckPunctuationSpacing(prev, curr, options))
        return *res;
    if (auto res = CheckUnaryAndIncrementSpacing(tokens, prevIdx, currIdx))
        return *res;
    if (auto res = CheckAtRefAndTemplateSpacing(tokens, prevIdx, currIdx, options))
        return *res;
    if (auto res = CheckParenAndBracketSpacing(tokens, prevIdx, currIdx, options))
        return *res;
    if (auto res = CheckBinaryAndColonSpacing(tokens, prevIdx, currIdx, options))
        return *res;
    return CheckWordAndLiteralSpacing(prev, curr);
}
} // namespace angel_lsp::features
