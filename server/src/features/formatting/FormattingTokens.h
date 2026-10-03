#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Categorization of lexical tokens in AngelScript source.
 */
enum class TokenType
{
    Identifier,
    Keyword,
    Number,
    StringLiteral,
    CharacterLiteral,
    LineComment,
    BlockComment,
    Preprocessor,
    OpenBrace,    // {
    CloseBrace,   // }
    OpenParen,    // (
    CloseParen,   // )
    OpenBracket,  // [
    CloseBracket, // ]
    Semicolon,    // ;
    Comma,        // ,
    Colon,        // :
    DoubleColon,  // ::
    Question,     // ?
    Dot,          // .
    At,           // @
    Ampersand,    // &
    Operator,     // binary/unary operators: =, ==, +, -, etc.
    Increment,    // ++
    Decrement,    // --
    EndOfFile
};

/**
 * @brief Representation of an individual token with positional and syntactic metadata.
 */
struct Token
{
    TokenType type = TokenType::EndOfFile;
    std::string text;
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t newlinesBefore = 0;
    bool isTemplateOpener = false;
    bool isTemplateCloser = false;
    bool isPaddedBrace = false;
    bool isUnterminated = false;
    bool isClangFormatOff = false;
};

/**
 * @brief Tokenizes AngelScript source text into an annotated token stream.
 * @param[in] src Source text.
 * @return Vector of annotated tokens.
 */
std::vector<Token> Tokenize(std::string_view src);

/**
 * @brief Checks if a token represents a type name or type qualifier.
 * @param[in] tok Token to check.
 * @return True if token can be a type preceding '@' or '&'.
 */
bool IsTypeLikePrecedingToken(const Token& tok);

/**
 * @brief Determines if an '@' token belongs to a type declarator (e.g. Type@ var).
 * @param[in] tokens Token stream.
 * @param[in] atIdx Index of the '@' token.
 * @return True if preceded by a type, false if unary operator or expression.
 */
bool IsHandleTypeDeclarator(const std::vector<Token>& tokens, size_t atIdx);

/**
 * @brief Determines if an '&' token belongs to a reference declarator (e.g. Type& ref).
 * @param[in] tokens Token stream.
 * @param[in] ampIdx Index of the '&' token.
 * @return True if preceded by a type, false if binary '&' or expression.
 */
bool IsReferenceTypeDeclarator(const std::vector<Token>& tokens, size_t ampIdx);

/**
 * @brief Tests if token type indicates a preceding expression boundary for a unary operator.
 * @param[in] type Token type.
 * @return True if operator following this type is unary.
 */
bool IsUnaryPrecedingTokenType(TokenType type);

/**
 * @brief Checks if an operator token at index `idx` is unary.
 * @param[in] tokens Token stream.
 * @param[in] idx Index of candidate token.
 * @return True if token is a unary operator.
 */
bool IsUnary(const std::vector<Token>& tokens, size_t idx);

/**
 * @brief Determines if a colon at `idx` is part of a case/default label or access specifier.
 * @param[in] tokens Token stream.
 * @param[in] idx Index of colon token.
 * @return True if colon terminates a label or specifier.
 */
bool IsAccessSpecifierOrLabelColon(const std::vector<Token>& tokens, size_t idx);
} // namespace angel_lsp::features
