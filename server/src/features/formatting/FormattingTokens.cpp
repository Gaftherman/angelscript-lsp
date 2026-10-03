#include "features/formatting/FormattingTokens.h"
#include "features/formatting/FormattingLexer.h"
#include <unordered_set>

namespace angel_lsp::features
{
namespace
{
std::vector<Token> SplitShiftOperators(const std::vector<Token>& tokens)
{
    std::vector<Token> splitTokens;
    splitTokens.reserve(tokens.size());
    for (const auto& tok : tokens)
    {
        if (tok.type == TokenType::Operator && tok.text == ">>")
        {
            splitTokens.push_back({TokenType::Operator, ">", tok.line, tok.column, tok.newlinesBefore});
            splitTokens.push_back({TokenType::Operator, ">", tok.line, tok.column + 1, 0});
        }
        else if (tok.type == TokenType::Operator && tok.text == ">>>")
        {
            splitTokens.push_back({TokenType::Operator, ">", tok.line, tok.column, tok.newlinesBefore});
            splitTokens.push_back({TokenType::Operator, ">", tok.line, tok.column + 1, 0});
            splitTokens.push_back({TokenType::Operator, ">", tok.line, tok.column + 2, 0});
        }
        else
        {
            splitTokens.push_back(tok);
        }
    }
    return splitTokens;
}

bool CanPrecedeTemplate(const Token& prev)
{
    return prev.type == TokenType::Identifier || prev.text == "cast" || prev.isTemplateCloser;
}

bool IsTemplateTerminator(TokenType type)
{
    return type == TokenType::Semicolon || type == TokenType::OpenBrace || type == TokenType::CloseBrace;
}

void TryIdentifyTemplateAt(std::vector<Token>& tokens, size_t t)
{
    if (t == 0 || tokens[t].text != "<" || !CanPrecedeTemplate(tokens[t - 1]))
        return;

    int depth = 1;
    size_t matchIdx = t + 1;
    bool valid = true;
    std::vector<size_t> closerIndices;

    while (matchIdx < tokens.size() && depth > 0)
    {
        if (IsTemplateTerminator(tokens[matchIdx].type))
        {
            valid = false;
            break;
        }
        if (tokens[matchIdx].text == "<")
        {
            depth++;
        }
        else if (tokens[matchIdx].text == ">")
        {
            depth--;
            closerIndices.push_back(matchIdx);
        }
        matchIdx++;
    }
    if (valid && depth == 0)
    {
        tokens[t].isTemplateOpener = true;
        for (size_t cIdx : closerIndices)
        {
            tokens[cIdx].isTemplateCloser = true;
        }
    }
}

void MarkClangFormatPragmas(std::vector<Token>& tokens)
{
    bool off = false;
    for (auto& tok : tokens)
    {
        if (tok.type == TokenType::LineComment || tok.type == TokenType::BlockComment)
        {
            if (tok.text.find("clang-format off") != std::string::npos)
            {
                off = true;
            }
            else if (tok.text.find("clang-format on") != std::string::npos)
            {
                tok.isClangFormatOff = false;
                off = false;
                continue;
            }
        }
        tok.isClangFormatOff = off;
    }
}

bool IsBuiltInTypeKeyword(std::string_view text)
{
    static const std::unordered_set<std::string_view> kTypeKeywords = {
        "auto", "void",  "bool",   "int",    "int8",   "int16", "int32",  "int64",
        "uint", "uint8", "uint16", "uint32", "uint64", "float", "double", "const"};
    return kTypeKeywords.contains(text);
}

bool IsDirectionKeyword(const Token& tok)
{
    return tok.type == TokenType::Keyword && (tok.text == "in" || tok.text == "out" || tok.text == "inout");
}
} // namespace

std::vector<Token> Tokenize(std::string_view src)
{
    auto raw = ScanRawTokens(src);
    auto tokens = SplitShiftOperators(raw);
    for (size_t t = 0; t < tokens.size(); ++t)
    {
        TryIdentifyTemplateAt(tokens, t);
    }
    MarkClangFormatPragmas(tokens);
    return tokens;
}

bool IsTypeLikePrecedingToken(const Token& tok)
{
    if (tok.type == TokenType::Identifier || tok.isTemplateCloser)
        return true;
    return tok.type == TokenType::Keyword && IsBuiltInTypeKeyword(tok.text);
}

bool IsHandleTypeDeclarator(const std::vector<Token>& tokens, size_t atIdx)
{
    size_t p = atIdx;
    while (p > 0 && tokens[p - 1].type == TokenType::At)
    {
        p--;
    }
    return p > 0 && IsTypeLikePrecedingToken(tokens[p - 1]);
}

bool IsStatementStartBefore(const std::vector<Token>& tokens, size_t p)
{
    if (p == 1)
        return true;
    const TokenType prevType = tokens[p - 2].type;
    return prevType == TokenType::Semicolon || prevType == TokenType::OpenBrace || prevType == TokenType::CloseBrace;
}

bool IsIdentifierReferenceDecl(const std::vector<Token>& tokens, size_t p, size_t ampIdx)
{
    if (p >= 2 && tokens[p - 2].text == "const")
        return true;
    if (ampIdx + 1 < tokens.size() && IsDirectionKeyword(tokens[ampIdx + 1]))
        return true;
    return IsStatementStartBefore(tokens, p) && ampIdx + 2 < tokens.size() &&
           tokens[ampIdx + 1].type == TokenType::Identifier && tokens[ampIdx + 2].text == "=";
}

bool IsReferenceTypeDeclarator(const std::vector<Token>& tokens, size_t ampIdx)
{
    if (ampIdx == 0 || tokens[ampIdx].text != "&")
        return false;

    size_t p = ampIdx;
    while (p > 0 && tokens[p - 1].type == TokenType::At)
    {
        p--;
    }
    if (p == 0)
        return false;

    const auto& typeTok = tokens[p - 1];
    if (p < ampIdx)
        return IsTypeLikePrecedingToken(typeTok);
    if (typeTok.isTemplateCloser || (typeTok.type == TokenType::Keyword && IsBuiltInTypeKeyword(typeTok.text)))
        return true;
    if (typeTok.type != TokenType::Identifier)
        return false;

    return IsIdentifierReferenceDecl(tokens, p, ampIdx);
}

bool IsUnaryPrecedingTokenType(TokenType type)
{
    switch (type)
    {
    case TokenType::OpenParen:
    case TokenType::OpenBracket:
    case TokenType::Comma:
    case TokenType::Semicolon:
    case TokenType::Question:
    case TokenType::Colon:
    case TokenType::Operator:
    case TokenType::Increment:
    case TokenType::Decrement:
        return true;
    default:
        return false;
    }
}

bool IsUnary(const std::vector<Token>& tokens, size_t idx)
{
    if (tokens[idx].text != "+" && tokens[idx].text != "-")
        return false;
    if (idx == 0)
        return true;
    const auto& prev = tokens[idx - 1];
    if (IsUnaryPrecedingTokenType(prev.type))
        return true;
    return prev.type == TokenType::Keyword &&
           (prev.text == "return" || prev.text == "case" || prev.text == "throw" || prev.text == "is");
}

bool IsAccessSpecifierOrLabelColon(const std::vector<Token>& tokens, size_t idx)
{
    if (tokens[idx].type != TokenType::Colon || idx == 0)
        return false;
    for (int k = static_cast<int>(idx) - 1; k >= 0; --k)
    {
        if (tokens[k].type == TokenType::Semicolon || tokens[k].type == TokenType::OpenBrace ||
            tokens[k].type == TokenType::CloseBrace)
        {
            break;
        }
        if (tokens[k].type == TokenType::Keyword &&
            (tokens[k].text == "case" || tokens[k].text == "default" || tokens[k].text == "public" ||
             tokens[k].text == "private" || tokens[k].text == "protected"))
        {
            return true;
        }
    }
    return false;
}
} // namespace angel_lsp::features
