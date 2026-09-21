#include "features/formatting/FormattingHandler.h"
#include "parser/Keywords.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
namespace
{
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
    Arrow,        // ->
    At,           // @
    Operator,     // binary/unary operators: =, ==, +, -, etc.
    Increment,    // ++
    Decrement,    // --
    EndOfFile
};

struct Token
{
    TokenType type = TokenType::EndOfFile;
    std::string text;
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t newlinesBefore = 0;
    bool isTemplateOpener = false;
    bool isTemplateCloser = false;

    /**
     * @brief For a brace sharing its line: whether its contents are padded with spaces.
     *
     * `{ Log(a); }` for a lambda body, `{1, 2}` for a list. Both are value braces and both
     * stay on their line, but one holds statements and the other holds elements, and the
     * two read differently enough that one rule for both is wrong either way round. Set
     * while the braces are being classified, since that is the only pass that knows.
     */
    bool isPaddedBrace = false;

    /**
     * @brief A string or character literal whose closing quote is missing.
     *
     * `"` and `'` both end at the line break, matching the default engine - multiline
     * strings are off unless the host turns them on. What is left is a token that ran into
     * the end of its line, and joining the next line onto it would pull the following code
     * *inside* the literal. Common enough to matter: it is the state every string is in
     * while it is being typed.
     */
    bool isUnterminated = false;
};

// Was a 65-word copy that omitted `foreach` and `using` - so neither was ever coloured -
// and included `with`, which is JavaScript's keyword and appears nowhere in this grammar.
static const std::unordered_set<std::string_view> kKeywords = []
{
    std::unordered_set<std::string_view> all;
    all.insert(parser::keywords::k_reserved.begin(), parser::keywords::k_reserved.end());
    all.insert(parser::keywords::k_contextual.begin(), parser::keywords::k_contextual.end());
    return all;
}();

static const std::unordered_set<std::string_view> kControlKeywords = {"if", "for", "while", "switch", "catch", "with"};

static constexpr std::string_view kTwoCharOperators[] = {
    "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "^^"};

struct TokenizeState
{
    std::string_view src;
    size_t i = 0;
    uint32_t curLine = 0;
    uint32_t curCol = 0;
    uint32_t newlines = 0;
    std::vector<Token> tokens;
};

/**
 * @brief Consumes whitespace characters and tracks line / column / newline counts.
 * @param[in,out] state Active tokenizer state.
 * @return True if whitespace was consumed, false otherwise.
 */
bool TryConsumeWhitespace(TokenizeState& state)
{
    char c = state.src[state.i];
    if (c == '\r' || c == '\n')
    {
        if (c == '\r' && state.i + 1 < state.src.size() && state.src[state.i + 1] == '\n')
        {
            state.i += 2;
        }
        else
        {
            state.i++;
        }
        state.curLine++;
        state.curCol = 0;
        state.newlines++;
        return true;
    }
    if (c == ' ' || c == '\t' || c == '\v' || c == '\f')
    {
        state.curCol++;
        state.i++;
        return true;
    }
    return false;
}

/**
 * @brief Consumes a single-line comment beginning with `//`.
 * @param[in,out] state Active tokenizer state.
 * @return True if a line comment was consumed, false otherwise.
 */
bool TryConsumeLineComment(TokenizeState& state)
{
    char c = state.src[state.i];
    if (c == '/' && state.i + 1 < state.src.size() && state.src[state.i + 1] == '/')
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        while (state.i < state.src.size() && state.src[state.i] != '\r' && state.src[state.i] != '\n')
        {
            state.i++;
            state.curCol++;
        }
        state.tokens.push_back({TokenType::LineComment, std::string(state.src.substr(start, state.i - start)),
                                state.curLine, startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

/**
 * @brief Consumes a block comment beginning with `/*`.
 * @param[in,out] state Active tokenizer state.
 * @return True if a block comment was consumed, false otherwise.
 */
bool TryConsumeBlockComment(TokenizeState& state)
{
    char c = state.src[state.i];
    if (c == '/' && state.i + 1 < state.src.size() && state.src[state.i + 1] == '*')
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        state.i += 2;
        state.curCol += 2;
        while (state.i < state.src.size() &&
               !(state.src[state.i] == '*' && state.i + 1 < state.src.size() && state.src[state.i + 1] == '/'))
        {
            if (state.src[state.i] == '\n')
            {
                state.curLine++;
                state.curCol = 0;
            }
            else
            {
                state.curCol++;
            }
            state.i++;
        }
        if (state.i < state.src.size())
        {
            state.i += 2;
            state.curCol += 2;
        }
        state.tokens.push_back({TokenType::BlockComment, std::string(state.src.substr(start, state.i - start)),
                                state.curLine, startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

/**
 * @brief Consumes a preprocessor directive starting with `#`.
 * @param[in,out] state Active tokenizer state.
 * @return True if a preprocessor directive was consumed, false otherwise.
 */
bool TryConsumePreprocessor(TokenizeState& state)
{
    char c = state.src[state.i];
    if (c == '#' && (state.curCol == 0 || state.tokens.empty() || state.newlines > 0))
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        while (state.i < state.src.size() && state.src[state.i] != '\r' && state.src[state.i] != '\n')
        {
            if (state.src[state.i] == '\\' && state.i + 1 < state.src.size() &&
                (state.src[state.i + 1] == '\r' || state.src[state.i + 1] == '\n'))
            {
                if (state.src[state.i + 1] == '\r' && state.i + 2 < state.src.size() && state.src[state.i + 2] == '\n')
                {
                    state.i += 3;
                }
                else
                {
                    state.i += 2;
                }
                state.curLine++;
                state.curCol = 0;
                continue;
            }
            state.i++;
            state.curCol++;
        }
        state.tokens.push_back({TokenType::Preprocessor, std::string(state.src.substr(start, state.i - start)),
                                state.curLine, startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

/**
 * @brief Consumes a multi-line raw string literal delimited by triple quotes `"""`.
 * @param[in,out] state Active tokenizer state.
 * @return True if a raw string literal was consumed, false otherwise.
 */
bool TryConsumeRawString(TokenizeState& state)
{
    char c = state.src[state.i];
    if (c == '"' && state.i + 2 < state.src.size() && state.src[state.i + 1] == '"' && state.src[state.i + 2] == '"')
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        state.i += 3;
        state.curCol += 3;
        while (state.i < state.src.size() && !(state.src[state.i] == '"' && state.i + 2 < state.src.size() &&
                                               state.src[state.i + 1] == '"' && state.src[state.i + 2] == '"'))
        {
            if (state.src[state.i] == '\n')
            {
                state.curLine++;
                state.curCol = 0;
            }
            else
            {
                state.curCol++;
            }
            state.i++;
        }
        if (state.i < state.src.size())
        {
            state.i += 3;
            state.curCol += 3;
        }
        state.tokens.push_back({TokenType::StringLiteral, std::string(state.src.substr(start, state.i - start)),
                                state.curLine, startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

/**
 * @brief Consumes a single-quote or double-quote character/string literal.
 * @param[in,out] state Active tokenizer state.
 * @param[in] quoteChar Delimiter character (`'` or `"`).
 * @param[in] tokenType Target token type.
 * @return True if a quoted literal was consumed, false otherwise.
 */
bool TryConsumeQuotedLiteral(TokenizeState& state, char quoteChar, TokenType tokenType)
{
    if (state.src[state.i] != quoteChar)
    {
        return false;
    }
    size_t start = state.i;
    uint32_t startCol = state.curCol;
    state.i++;
    state.curCol++;
    while (state.i < state.src.size() && state.src[state.i] != quoteChar)
    {
        if (state.src[state.i] == '\\' && state.i + 1 < state.src.size())
        {
            state.i += 2;
            state.curCol += 2;
            continue;
        }
        if (state.src[state.i] == '\n' || state.src[state.i] == '\r')
        {
            break;
        }
        state.i++;
        state.curCol++;
    }
    const bool closed = state.i < state.src.size() && state.src[state.i] == quoteChar;
    if (closed)
    {
        state.i++;
        state.curCol++;
    }
    Token tok{tokenType, std::string(state.src.substr(start, state.i - start)), state.curLine, startCol,
              state.newlines};
    tok.isUnterminated = !closed;
    state.tokens.push_back(std::move(tok));
    state.newlines = 0;
    return true;
}

/**
 * @brief Consumes UTF-8 byte sequences or identifiers and keywords.
 * @param[in,out] state Active tokenizer state.
 * @return True if an identifier, keyword, or non-ASCII sequence was consumed.
 */
bool TryConsumeIdentifierOrNonAscii(TokenizeState& state)
{
    char c = state.src[state.i];
    if (static_cast<unsigned char>(c) >= 0x80)
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        while (state.i < state.src.size() && static_cast<unsigned char>(state.src[state.i]) >= 0x80)
        {
            state.i++;
            state.curCol++;
        }
        state.tokens.push_back({TokenType::Identifier, std::string(state.src.substr(start, state.i - start)),
                                state.curLine, startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
    {
        size_t start = state.i;
        uint32_t startCol = state.curCol;
        while (state.i < state.src.size() &&
               (std::isalnum(static_cast<unsigned char>(state.src[state.i])) || state.src[state.i] == '_'))
        {
            state.i++;
            state.curCol++;
        }
        std::string text(state.src.substr(start, state.i - start));
        TokenType tt = (kKeywords.contains(text)) ? TokenType::Keyword : TokenType::Identifier;
        state.tokens.push_back({tt, std::move(text), state.curLine, startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

/**
 * @brief Consumes exponent notation (`e+10`, `E-3`) in numeric literals.
 * @param[in,out] state Active tokenizer state.
 */
void ConsumeDecimalExponent(TokenizeState& state)
{
    if (state.i < state.src.size() && (state.src[state.i] == 'e' || state.src[state.i] == 'E'))
    {
        state.i++;
        state.curCol++;
        if (state.i < state.src.size() && (state.src[state.i] == '+' || state.src[state.i] == '-'))
        {
            state.i++;
            state.curCol++;
        }
        while (state.i < state.src.size() && std::isdigit(static_cast<unsigned char>(state.src[state.i])))
        {
            state.i++;
            state.curCol++;
        }
    }
}

/**
 * @brief Consumes digits matching the given prefix base ('x', 'b', or 'o').
 * @param[in,out] state Active tokenizer state.
 * @param[in] prefix Lowercase prefix character.
 */
void ConsumePrefixedDigits(TokenizeState& state, char prefix)
{
    state.i += 2;
    state.curCol += 2;
    switch (prefix)
    {
    case 'x':
        while (state.i < state.src.size() && std::isxdigit(static_cast<unsigned char>(state.src[state.i])))
        {
            state.i++;
            state.curCol++;
        }
        break;
    case 'b':
        while (state.i < state.src.size() && (state.src[state.i] == '0' || state.src[state.i] == '1'))
        {
            state.i++;
            state.curCol++;
        }
        break;
    case 'o':
        while (state.i < state.src.size() && (state.src[state.i] >= '0' && state.src[state.i] <= '7'))
        {
            state.i++;
            state.curCol++;
        }
        break;
    default:
        break;
    }
}

/**
 * @brief Consumes hex, binary, or octal prefixed numbers (`0x`, `0b`, `0o`).
 * @param[in,out] state Active tokenizer state.
 * @return True if a prefixed number base was consumed.
 */
bool TryConsumePrefixedNumber(TokenizeState& state)
{
    char c = state.src[state.i];
    if (c != '0' || state.i + 1 >= state.src.size())
    {
        return false;
    }
    char p = static_cast<char>(std::tolower(static_cast<unsigned char>(state.src[state.i + 1])));
    if (p == 'x' || p == 'b' || p == 'o')
    {
        ConsumePrefixedDigits(state, p);
        return true;
    }
    return false;
}

/**
 * @brief Consumes decimal integers and floating-point numeric sequences.
 * @param[in,out] state Active tokenizer state.
 */
void ConsumeDecimalNumber(TokenizeState& state)
{
    while (state.i < state.src.size() && std::isdigit(static_cast<unsigned char>(state.src[state.i])))
    {
        state.i++;
        state.curCol++;
    }
    if (state.i < state.src.size() && state.src[state.i] == '.' && state.i + 1 < state.src.size() &&
        std::isdigit(static_cast<unsigned char>(state.src[state.i + 1])))
    {
        state.i++;
        state.curCol++;
        while (state.i < state.src.size() && std::isdigit(static_cast<unsigned char>(state.src[state.i])))
        {
            state.i++;
            state.curCol++;
        }
    }
    ConsumeDecimalExponent(state);
}

/**
 * @brief Consumes numeric literals including hex, binary, octal, and floating point.
 * @param[in,out] state Active tokenizer state.
 * @return True if a numeric literal was consumed, false otherwise.
 */
bool TryConsumeNumber(TokenizeState& state)
{
    char c = state.src[state.i];
    if (!std::isdigit(static_cast<unsigned char>(c)))
    {
        return false;
    }
    size_t start = state.i;
    uint32_t startCol = state.curCol;
    if (!TryConsumePrefixedNumber(state))
    {
        ConsumeDecimalNumber(state);
    }
    while (state.i < state.src.size() &&
           (std::isalnum(static_cast<unsigned char>(state.src[state.i])) || state.src[state.i] == '_'))
    {
        state.i++;
        state.curCol++;
    }
    state.tokens.push_back({TokenType::Number, std::string(state.src.substr(start, state.i - start)), state.curLine,
                            startCol, state.newlines});
    state.newlines = 0;
    return true;
}

/**
 * @brief Recognizes 2-char operator token types.
 * @param[in] op2 Two-character slice.
 * @return Detected token type or `TokenType::EndOfFile` if unrecognized.
 */
TokenType ClassifyTwoCharOperator(std::string_view op2)
{
    if (op2 == "++")
        return TokenType::Increment;
    if (op2 == "--")
        return TokenType::Decrement;
    if (op2 == "::")
        return TokenType::DoubleColon;
    if (op2 == "->")
        return TokenType::Arrow;
    for (std::string_view candidate : kTwoCharOperators)
    {
        if (op2 == candidate)
        {
            return TokenType::Operator;
        }
    }
    return TokenType::EndOfFile;
}

/**
 * @brief Consumes multi-character operators (4-char, 3-char, 2-char).
 * @param[in,out] state Active tokenizer state.
 * @return True if a multi-char operator was consumed, false otherwise.
 */
bool TryConsumeMultiCharOperator(TokenizeState& state)
{
    if (state.i + 3 < state.src.size() && state.src.substr(state.i, 4) == ">>>=")
    {
        state.tokens.push_back({TokenType::Operator, ">>>=", state.curLine, state.curCol, state.newlines});
        state.i += 4;
        state.curCol += 4;
        state.newlines = 0;
        return true;
    }
    if (state.i + 2 < state.src.size())
    {
        std::string_view op3 = state.src.substr(state.i, 3);
        const bool op3IsWordOperator = (op3 == "!is");
        const bool op3EndsAtWordBoundary =
            !op3IsWordOperator || state.i + 3 >= state.src.size() ||
            (std::isalnum(static_cast<unsigned char>(state.src[state.i + 3])) == 0 && state.src[state.i + 3] != '_');

        if ((op3 == "<<=" || op3 == ">>=" || op3 == ">>>" || op3IsWordOperator) && op3EndsAtWordBoundary)
        {
            state.tokens.push_back(
                {TokenType::Operator, std::string(op3), state.curLine, state.curCol, state.newlines});
            state.i += 3;
            state.curCol += 3;
            state.newlines = 0;
            return true;
        }
    }
    if (state.i + 1 < state.src.size())
    {
        TokenType tt = ClassifyTwoCharOperator(state.src.substr(state.i, 2));
        if (tt != TokenType::EndOfFile)
        {
            state.tokens.push_back(
                {tt, std::string(state.src.substr(state.i, 2)), state.curLine, state.curCol, state.newlines});
            state.i += 2;
            state.curCol += 2;
            state.newlines = 0;
            return true;
        }
    }
    return false;
}

/**
 * @brief Consumes a single-character punctuation or operator token.
 * @param[in,out] state Active tokenizer state.
 */
void ConsumeSingleCharToken(TokenizeState& state)
{
    char c = state.src[state.i];
    TokenType tt = TokenType::Operator;
    switch (c)
    {
    case '{':
        tt = TokenType::OpenBrace;
        break;
    case '}':
        tt = TokenType::CloseBrace;
        break;
    case '(':
        tt = TokenType::OpenParen;
        break;
    case ')':
        tt = TokenType::CloseParen;
        break;
    case '[':
        tt = TokenType::OpenBracket;
        break;
    case ']':
        tt = TokenType::CloseBracket;
        break;
    case ';':
        tt = TokenType::Semicolon;
        break;
    case ',':
        tt = TokenType::Comma;
        break;
    case ':':
        tt = TokenType::Colon;
        break;
    case '?':
        tt = TokenType::Question;
        break;
    case '.':
        tt = TokenType::Dot;
        break;
    case '@':
        tt = TokenType::At;
        break;
    default:
        tt = TokenType::Operator;
        break;
    }
    state.tokens.push_back({tt, std::string(1, c), state.curLine, state.curCol, state.newlines});
    state.i++;
    state.curCol++;
    state.newlines = 0;
}

/**
 * @brief Splits compound shift operators `>>` and `>>>` into individual `>` tokens.
 * @param[in] tokens Token list from initial tokenization.
 * @return Transformed token vector with separate `>` operators.
 */
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

/**
 * @brief Checks if a token can legally precede a template opening bracket.
 * @param[in] prev Preceding token.
 * @return True if candidate can precede `<...>` template arguments.
 */
bool CanPrecedeTemplate(const Token& prev)
{
    return prev.type == TokenType::Identifier || prev.text == "cast" || prev.isTemplateCloser;
}

/**
 * @brief Checks if a token terminates a template bracket scan.
 * @param[in] type Token type.
 * @return True if token halts template identification.
 */
bool IsTemplateTerminator(TokenType type)
{
    return type == TokenType::Semicolon || type == TokenType::OpenBrace || type == TokenType::CloseBrace;
}

/**
 * @brief Evaluates whether a `<` token at index `t` represents a template opener.
 * @param[in,out] tokens Token stream.
 * @param[in] t Index of the candidate `<` token.
 */
void TryIdentifyTemplateAt(std::vector<Token>& tokens, size_t t)
{
    if (t == 0 || tokens[t].text != "<" || !CanPrecedeTemplate(tokens[t - 1]))
    {
        return;
    }
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

/**
 * @brief Scans token stream to flag matched template opening and closing brackets.
 * @param[in,out] tokens Token stream.
 */
void IdentifyTemplates(std::vector<Token>& tokens)
{
    for (size_t t = 0; t < tokens.size(); ++t)
    {
        TryIdentifyTemplateAt(tokens, t);
    }
}

/**
 * @brief Tokenizes AngelScript source text into an annotated token list.
 * @param[in] src Source text.
 * @return Vector of tokens.
 */
std::vector<Token> Tokenize(std::string_view src)
{
    TokenizeState state{src};

    while (state.i < state.src.size())
    {
        if (TryConsumeWhitespace(state) || TryConsumeLineComment(state) || TryConsumeBlockComment(state) ||
            TryConsumePreprocessor(state) || TryConsumeRawString(state) ||
            TryConsumeQuotedLiteral(state, '"', TokenType::StringLiteral) ||
            TryConsumeQuotedLiteral(state, '\'', TokenType::CharacterLiteral) ||
            TryConsumeIdentifierOrNonAscii(state) || TryConsumeNumber(state) || TryConsumeMultiCharOperator(state))
        {
            continue;
        }
        ConsumeSingleCharToken(state);
    }

    auto tokens = SplitShiftOperators(state.tokens);
    IdentifyTemplates(tokens);
    return tokens;
}

/**
 * @brief Tests if token type indicates a preceding expression boundary for a unary operator.
 * @param[in] type Token type.
 * @return True if operator following this type is unary.
 */
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

/**
 * @brief Tests if keyword text indicates a preceding expression boundary for a unary operator.
 * @param[in] kw Keyword text.
 * @return True if operator following this keyword is unary.
 */
bool IsUnaryPrecedingKeyword(std::string_view kw)
{
    return kw == "return" || kw == "case" || kw == "throw" || kw == "is";
}

/**
 * @brief Checks if a `+` or `-` operator token at index `idx` is unary.
 * @param[in] tokens Token stream.
 * @param[in] idx Index of candidate token.
 * @return True if the token is a unary operator.
 */
bool IsUnary(const std::vector<Token>& tokens, size_t idx)
{
    if (tokens[idx].text != "+" && tokens[idx].text != "-")
    {
        return false;
    }
    if (idx == 0)
    {
        return true;
    }
    const auto& prev = tokens[idx - 1];
    if (IsUnaryPrecedingTokenType(prev.type))
    {
        return true;
    }
    return prev.type == TokenType::Keyword && IsUnaryPrecedingKeyword(prev.text);
}

/**
 * @brief Determines if a colon at `idx` is part of a case/default label or access specifier.
 * @param[in] tokens Token stream.
 * @param[in] idx Index of colon token.
 * @return True if colon terminates a label or specifier.
 */
bool IsAccessSpecifierOrLabelColon(const std::vector<Token>& tokens, size_t idx)
{
    if (tokens[idx].type != TokenType::Colon || idx == 0)
    {
        return false;
    }
    for (int k = static_cast<int>(idx) - 1; k >= 0; --k)
    {
        if (tokens[k].type == TokenType::Semicolon || tokens[k].type == TokenType::OpenBrace ||
            tokens[k].type == TokenType::CloseBrace)
        {
            break;
        }
        if (tokens[k].type == TokenType::Keyword)
        {
            if (tokens[k].text == "case" || tokens[k].text == "default" || tokens[k].text == "public" ||
                tokens[k].text == "private" || tokens[k].text == "protected")
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Tests if token type represents closing punctuation or separator.
 * @param[in] type Token type.
 * @return True if punctuation suppresses leading space.
 */
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
    case TokenType::Arrow:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Tests if token type represents opening punctuation or member access.
 * @param[in] type Token type.
 * @return True if punctuation suppresses trailing space.
 */
bool IsOpeningOrMemberPunctuation(TokenType type)
{
    switch (type)
    {
    case TokenType::OpenParen:
    case TokenType::OpenBracket:
    case TokenType::Dot:
    case TokenType::DoubleColon:
    case TokenType::Arrow:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Evaluates spacing rules between adjacent punctuation tokens.
 * @param[in] prev Previous token.
 * @param[in] curr Current token.
 * @return Boolean decision if determined by punctuation rules, std::nullopt otherwise.
 */
std::optional<bool> CheckPunctuationSpacing(const Token& prev, const Token& curr)
{
    if (curr.type == TokenType::Comma && (prev.type == TokenType::Comma || prev.type == TokenType::OpenBrace))
    {
        return true;
    }
    if (IsClosingOrSeparatorPunctuation(curr.type))
    {
        return false;
    }
    if (IsOpeningOrMemberPunctuation(prev.type))
    {
        return false;
    }
    if (prev.type == TokenType::OpenBrace)
    {
        return prev.isPaddedBrace;
    }
    if (curr.type == TokenType::CloseBrace)
    {
        return curr.isPaddedBrace;
    }
    return std::nullopt;
}

/**
 * @brief Evaluates spacing rules for increment, decrement, and unary operators.
 * @param[in] tokens Token stream.
 * @param[in] prevIdx Index of previous token.
 * @param[in] currIdx Index of current token.
 * @return Boolean decision if determined by unary/increment rules, std::nullopt otherwise.
 */
std::optional<bool> CheckUnaryAndIncrementSpacing(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx)
{
    const auto& prev = tokens[prevIdx];
    const auto& curr = tokens[currIdx];

    if (curr.type == TokenType::Increment || curr.type == TokenType::Decrement)
    {
        if (prev.type == TokenType::Identifier || prev.type == TokenType::CloseBracket ||
            prev.type == TokenType::CloseParen)
        {
            return false;
        }
    }
    if (prev.type == TokenType::Increment || prev.type == TokenType::Decrement)
    {
        return false;
    }
    if (prev.text == "!" || prev.text == "~")
    {
        return false;
    }
    if (IsUnary(tokens, prevIdx))
    {
        return false;
    }
    if (IsUnary(tokens, currIdx))
    {
        if (prev.type == TokenType::OpenParen || prev.type == TokenType::OpenBracket)
        {
            return false;
        }
        return true;
    }
    return std::nullopt;
}

/**
 * @brief Evaluates spacing rules for handle `@` and template brackets.
 * @param[in] prev Previous token.
 * @param[in] curr Current token.
 * @return Boolean decision if determined by `@` or template rules, std::nullopt otherwise.
 */
std::optional<bool> CheckAtAndTemplateSpacing(const Token& prev, const Token& curr)
{
    if (curr.type == TokenType::At)
    {
        return false;
    }
    if (prev.type == TokenType::At)
    {
        return curr.type == TokenType::Identifier || curr.type == TokenType::Keyword;
    }
    if (curr.isTemplateOpener || prev.isTemplateOpener || curr.isTemplateCloser)
    {
        return false;
    }
    if (prev.isTemplateCloser)
    {
        return curr.type == TokenType::Identifier || curr.type == TokenType::Keyword ||
               curr.type == TokenType::OpenParen || curr.type == TokenType::Operator;
    }
    return std::nullopt;
}

/**
 * @brief Tests whether token can be followed immediately by call parentheses without space.
 * @param[in] tok Preceding token.
 * @return True if function call or instantiation syntax suppresses space before `(`.
 */
bool IsCallLikePrecedingToken(const Token& tok)
{
    return tok.type == TokenType::Identifier || tok.text == "super" || tok.text == "this" || tok.text == "cast" ||
           tok.text == "function";
}

/**
 * @brief Evaluates spacing rules for parentheses and indexing brackets.
 * @param[in] prev Previous token.
 * @param[in] curr Current token.
 * @return Boolean decision if determined by paren/bracket rules, std::nullopt otherwise.
 */
std::optional<bool> CheckParenAndBracketSpacing(const Token& prev, const Token& curr)
{
    if (curr.type == TokenType::OpenParen)
    {
        if (prev.type == TokenType::Keyword && kControlKeywords.contains(prev.text))
        {
            return true;
        }
        if (IsCallLikePrecedingToken(prev))
        {
            return false;
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

/**
 * @brief Evaluates spacing rules for binary operators, colons, and ternary operators.
 * @param[in] tokens Token stream.
 * @param[in] prevIdx Index of previous token.
 * @param[in] currIdx Index of current token.
 * @return Boolean decision if determined by binary/colon rules, std::nullopt otherwise.
 */
std::optional<bool> CheckBinaryAndColonSpacing(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx)
{
    const auto& prev = tokens[prevIdx];
    const auto& curr = tokens[currIdx];

    if (curr.type == TokenType::Colon)
    {
        return !IsAccessSpecifierOrLabelColon(tokens, currIdx);
    }
    if (prev.type == TokenType::Comma || prev.type == TokenType::Semicolon)
    {
        return true;
    }
    if ((curr.type == TokenType::Operator && !curr.isTemplateOpener && !curr.isTemplateCloser) ||
        (prev.type == TokenType::Operator && !prev.isTemplateOpener && !prev.isTemplateCloser))
    {
        return true;
    }
    if (curr.type == TokenType::Question || prev.type == TokenType::Question || prev.type == TokenType::Colon)
    {
        return true;
    }
    return std::nullopt;
}

/**
 * @brief Tests if token type represents an identifier, number, or string literal.
 * @param[in] type Token type.
 * @return True if type is identifier, number, or string literal.
 */
bool IsWordOrLiteralType(TokenType type)
{
    return type == TokenType::Identifier || type == TokenType::Number || type == TokenType::StringLiteral;
}

/**
 * @brief Tests if token type represents an identifier or keyword.
 * @param[in] type Token type.
 * @return True if type is identifier or keyword.
 */
bool IsWordOrKeywordType(TokenType type)
{
    return type == TokenType::Identifier || type == TokenType::Keyword;
}

/**
 * @brief Tests if token type represents a line or block comment.
 * @param[in] type Token type.
 * @return True if type is a comment.
 */
bool IsCommentToken(TokenType type)
{
    return type == TokenType::LineComment || type == TokenType::BlockComment;
}

/**
 * @brief Evaluates spacing rules for words, literals, and comments.
 * @param[in] prev Previous token.
 * @param[in] curr Current token.
 * @return True if a space is required, false otherwise.
 */
bool CheckWordAndLiteralSpacing(const Token& prev, const Token& curr)
{
    if (prev.type == TokenType::Keyword || curr.type == TokenType::Keyword)
    {
        return true;
    }
    if (prev.type == TokenType::Identifier && IsWordOrLiteralType(curr.type))
    {
        return true;
    }
    if (prev.type == TokenType::CloseParen && (IsWordOrKeywordType(curr.type) || curr.type == TokenType::OpenBrace))
    {
        return true;
    }
    if (prev.type == TokenType::CloseBracket && IsWordOrKeywordType(curr.type))
    {
        return true;
    }
    if (IsCommentToken(curr.type))
    {
        return true;
    }
    return (prev.type == TokenType::Number || prev.type == TokenType::StringLiteral) && IsWordOrLiteralType(curr.type);
}

/**
 * @brief Determines whether a space should be inserted between two adjacent tokens.
 * @param[in] tokens Token stream.
 * @param[in] prevIdx Index of left-hand token.
 * @param[in] currIdx Index of right-hand token.
 * @return True if a whitespace separator should be emitted.
 */
bool NeedsSpaceBetween(const std::vector<Token>& tokens, size_t prevIdx, size_t currIdx)
{
    const auto& prev = tokens[prevIdx];
    const auto& curr = tokens[currIdx];

    if (auto res = CheckPunctuationSpacing(prev, curr))
    {
        return *res;
    }
    if (auto res = CheckUnaryAndIncrementSpacing(tokens, prevIdx, currIdx))
    {
        return *res;
    }
    if (auto res = CheckAtAndTemplateSpacing(prev, curr))
    {
        return *res;
    }
    if (auto res = CheckParenAndBracketSpacing(prev, curr))
    {
        return *res;
    }
    if (auto res = CheckBinaryAndColonSpacing(tokens, prevIdx, currIdx))
    {
        return *res;
    }
    return CheckWordAndLiteralSpacing(prev, curr);
}

/**
 * @brief Generates indentation whitespace for a given nesting depth.
 * @param[in] level Scope indent depth.
 * @param[in] options LSP formatting configuration.
 * @return Formatted indentation string.
 */
std::string MakeIndent(int level, const lsp::FormattingOptions& options)
{
    if (level <= 0)
    {
        return "";
    }
    uint32_t tabSize = options.tabSize > 0 ? options.tabSize : 4;
    if (options.insertSpaces)
    {
        return std::string(static_cast<size_t>(level * tabSize), ' ');
    }
    return std::string(static_cast<size_t>(level), '\t');
}

enum class ScopeKind
{
    Generic,
    Switch,
    Enum,
    Class,
    Value ///< An initializer list or a lambda body - a brace that produces a value.
};

/**
 * @brief One open brace, and the paren/bracket nesting the source was at when it opened.
 */
struct ScopeEntry
{
    ScopeKind kind = ScopeKind::Generic;
    int parenDepthAtOpen = 0;
    int bracketDepthAtOpen = 0;
    bool paddedBrace = false; ///< Carried to the closing brace, which shares the answer.
};

struct LineInfo
{
    int indentLevel = 0;
    bool isPreprocessor = false;
    bool isBlankLine = false;
    std::vector<size_t> tokenIndices;
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
        {
            return;
        }
        const bool inCaseBody = insideCaseBody && CurrentScopeKind() == ScopeKind::Switch;
        currentLine.indentLevel = inCaseBody ? braceLevel + 1 : braceLevel;
    }
};

/**
 * @brief Preserves intentional blank lines between statements.
 * @param[in,out] state Active line builder state.
 * @param[in] tok Current token.
 */
void HandleBlankLines(LineBuilderState& state, const Token& tok)
{
    if (tok.newlinesBefore >= 2 && !state.lines.empty() && !state.lines.back().isBlankLine)
    {
        state.FlushCurrentLine();
        LineInfo blank;
        blank.isBlankLine = true;
        state.lines.push_back(std::move(blank));
    }
}

/**
 * @brief Tracks paren, bracket, and pending scope types across tokens.
 * @param[in,out] state Active line builder state.
 * @param[in] tok Current token.
 */
void UpdateBracketNesting(LineBuilderState& state, const Token& tok)
{
    if (tok.type == TokenType::Keyword)
    {
        if (tok.text == "switch")
        {
            state.pendingScope = ScopeKind::Switch;
        }
        else if (tok.text == "enum")
        {
            state.pendingScope = ScopeKind::Enum;
        }
        else if (tok.text == "class" || tok.text == "interface")
        {
            state.pendingScope = ScopeKind::Class;
        }
    }
    if (tok.type == TokenType::OpenParen)
    {
        state.parenDepth++;
    }
    else if (tok.type == TokenType::CloseParen && state.parenDepth > 0)
    {
        state.parenDepth--;
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

/**
 * @brief Tracks value contexts triggered by assignments or returns.
 * @param[in,out] state Active line builder state.
 * @param[in] tok Current token.
 */
void UpdateValueContext(LineBuilderState& state, const Token& tok)
{
    const int baseParen = state.scopeStack.empty() ? 0 : state.scopeStack.back().parenDepthAtOpen;
    const int baseBracket = state.scopeStack.empty() ? 0 : state.scopeStack.back().bracketDepthAtOpen;
    const bool atScopeNesting = (state.parenDepth == baseParen && state.bracketDepth == baseBracket);

    if (atScopeNesting && state.CurrentScopeKind() != ScopeKind::Value)
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

/**
 * @brief Emits preprocessor directives and metadata attribute blocks on their own lines.
 * @param[in,out] state Active line builder state.
 * @param[in] tokens Token stream.
 * @param[in,out] i Current token index.
 * @return True if preprocessor directive or metadata block was processed.
 */
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

/**
 * @brief Processes opening curly braces according to style rules and value contexts.
 * @param[in,out] state Active line builder state.
 * @param[in,out] tokens Token stream.
 * @param[in] i Current token index.
 * @param[in] braceStyle Configured brace placement style.
 */
void HandleOpenBrace(LineBuilderState& state, std::vector<Token>& tokens, size_t i, BraceStyle braceStyle)
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
        state.scopeStack.push_back({ScopeKind::Value, state.parenDepth, state.bracketDepth, padded});
        return;
    }

    if (braceStyle == BraceStyle::KAndR && !state.currentLine.tokenIndices.empty())
    {
        state.currentLine.tokenIndices.push_back(i);
        state.FlushCurrentLine();
    }
    else
    {
        state.FlushCurrentLine();
        LineInfo braceLine;
        braceLine.indentLevel = state.braceLevel;
        braceLine.tokenIndices.push_back(i);
        state.lines.push_back(std::move(braceLine));
    }

    state.scopeStack.push_back({state.pendingScope, state.parenDepth, state.bracketDepth});
    state.pendingScope = ScopeKind::Generic;
    state.insideCaseBody = false;
    state.inValueContext = false;
    state.braceLevel++;
}

/**
 * @brief Processes closing curly braces and attached semicolons.
 * @param[in,out] state Active line builder state.
 * @param[in,out] tokens Token stream.
 * @param[in,out] i Current token index.
 */
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
    state.braceLevel = std::max(0, state.braceLevel - 1);
    if (!state.scopeStack.empty())
    {
        state.scopeStack.pop_back();
    }
    state.insideCaseBody = false;
    state.inValueContext = false;

    LineInfo braceLine;
    braceLine.indentLevel = state.braceLevel;
    braceLine.tokenIndices.push_back(i);

    if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Semicolon)
    {
        i++;
        braceLine.tokenIndices.push_back(i);
    }
    state.lines.push_back(std::move(braceLine));
}

/**
 * @brief Formats `else` keywords matching the configured brace style.
 * @param[in,out] state Active line builder state.
 * @param[in] tokens Token stream.
 * @param[in] i Current token index.
 * @param[in] braceStyle Configured brace placement style.
 * @return True if token was `else` and processed.
 */
bool HandleElseKeyword(LineBuilderState& state, const std::vector<Token>& tokens, size_t i, BraceStyle braceStyle)
{
    if (tokens[i].type != TokenType::Keyword || tokens[i].text != "else")
    {
        return false;
    }
    state.FlushCurrentLine();
    if (braceStyle == BraceStyle::KAndR && !state.lines.empty() && state.lines.back().tokenIndices.size() == 1 &&
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

/**
 * @brief Formats `case` or `default` label statements in switch blocks.
 * @param[in,out] state Active line builder state.
 * @param[in] tok Candidate token.
 * @param[in] i Current token index.
 * @return True if candidate is `case` or `default` keyword.
 */
bool HandleCaseOrDefault(LineBuilderState& state, const Token& tok, size_t i)
{
    if (tok.type == TokenType::Keyword && (tok.text == "case" || tok.text == "default"))
    {
        state.FlushCurrentLine();
        state.insideCaseBody = false;
        state.currentLine.indentLevel = state.braceLevel;
        state.currentLine.tokenIndices.push_back(i);
        return true;
    }
    return false;
}

/**
 * @brief Formats class member access specifiers (`public:`, `private:`, `protected:`).
 * @param[in,out] state Active line builder state.
 * @param[in] tokens Token stream.
 * @param[in,out] i Current token index.
 * @return True if an access specifier was processed.
 */
bool HandleAccessSpecifier(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
{
    const auto& tok = tokens[i];
    if (tok.type == TokenType::Keyword && (tok.text == "public" || tok.text == "private" || tok.text == "protected"))
    {
        if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Colon)
        {
            state.FlushCurrentLine();
            state.currentLine.indentLevel = std::max(0, state.braceLevel - 1);
            state.currentLine.tokenIndices.push_back(i);
            i++;
            state.currentLine.tokenIndices.push_back(i);
            state.FlushCurrentLine();
            return true;
        }
    }
    return false;
}

/**
 * @brief Formats `else` branches, switch `case`/`default` labels, and class access specifiers.
 * @param[in,out] state Active line builder state.
 * @param[in] tokens Token stream.
 * @param[in,out] i Current token index.
 * @param[in] braceStyle Configured brace placement style.
 * @return True if a keyword branch or label was processed.
 */
bool HandleKeywordsAndLabels(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i,
                             BraceStyle braceStyle)
{
    return HandleElseKeyword(state, tokens, i, braceStyle) || HandleCaseOrDefault(state, tokens[i], i) ||
           HandleAccessSpecifier(state, tokens, i);
}

/**
 * @brief Consumes trailing inline line or block comments on the active line.
 * @param[in,out] state Active line builder state.
 * @param[in] tokens Token stream.
 * @param[in,out] i Current token index.
 */
void ConsumeTrailingComment(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
{
    if (i + 1 < tokens.size() && tokens[i + 1].newlinesBefore == 0 &&
        (tokens[i + 1].type == TokenType::LineComment || tokens[i + 1].type == TokenType::BlockComment))
    {
        i++;
        state.currentLine.tokenIndices.push_back(i);
    }
}

/**
 * @brief Tests if token represents a statement semicolon or enum comma terminator.
 * @param[in] state Active line builder state.
 * @param[in] tok Current token.
 * @return True if token marks the end of a statement or enum member.
 */
bool IsStatementOrEnumTerminator(const LineBuilderState& state, const Token& tok)
{
    if (state.parenDepth != 0 || state.bracketDepth != 0)
    {
        return false;
    }
    if (tok.type == TokenType::Semicolon && state.CurrentScopeKind() != ScopeKind::Value)
    {
        return true;
    }
    return tok.type == TokenType::Comma && state.CurrentScopeKind() == ScopeKind::Enum;
}

/**
 * @brief Checks if a statement or comment token triggers a line boundary.
 * @param[in,out] state Active line builder state.
 * @param[in] tokens Token stream.
 * @param[in,out] i Current token index.
 */
void HandleStatementOrCommentEnd(LineBuilderState& state, const std::vector<Token>& tokens, size_t& i)
{
    const auto& tok = tokens[i];
    if (tok.isUnterminated || tok.type == TokenType::LineComment)
    {
        state.FlushCurrentLine();
        return;
    }

    if (IsStatementOrEnumTerminator(state, tok))
    {
        ConsumeTrailingComment(state, tokens, i);
        state.FlushCurrentLine();
        return;
    }

    if (tok.type == TokenType::Colon && IsAccessSpecifierOrLabelColon(tokens, i))
    {
        state.FlushCurrentLine();
        if (state.CurrentScopeKind() == ScopeKind::Switch)
        {
            state.insideCaseBody = true;
        }
    }
}

/**
 * @brief Groups tokens into discrete formatted lines with associated indentation levels.
 * @param[in,out] tokens Token stream.
 * @param[in] braceStyle Configured brace placement style.
 * @return Vector of formatted line specifications.
 */
std::vector<LineInfo> BuildFormattedLines(std::vector<Token>& tokens, BraceStyle braceStyle)
{
    LineBuilderState state;

    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const auto& tok = tokens[i];
        HandleBlankLines(state, tok);
        UpdateBracketNesting(state, tok);
        UpdateValueContext(state, tok);

        if (HandlePreprocessorOrMetadata(state, tokens, i))
        {
            continue;
        }
        if (tok.type == TokenType::OpenBrace)
        {
            HandleOpenBrace(state, tokens, i, braceStyle);
            continue;
        }
        if (tok.type == TokenType::CloseBrace)
        {
            HandleCloseBrace(state, tokens, i);
            continue;
        }
        if (HandleKeywordsAndLabels(state, tokens, i, braceStyle))
        {
            continue;
        }

        state.BeginLineIfEmpty();
        state.currentLine.tokenIndices.push_back(i);
        HandleStatementOrCommentEnd(state, tokens, i);
    }

    state.FlushCurrentLine();
    return state.lines;
}

/**
 * @brief Extracts UTF-8 Byte Order Mark (BOM) if present.
 * @param[in,out] sourceCode Input source code.
 * @return BOM string view if found, empty view otherwise.
 */
std::string_view ExtractBom(std::string_view& sourceCode)
{
    if (sourceCode.size() >= 3 && static_cast<unsigned char>(sourceCode[0]) == 0xEF &&
        static_cast<unsigned char>(sourceCode[1]) == 0xBB && static_cast<unsigned char>(sourceCode[2]) == 0xBF)
    {
        std::string_view bom = sourceCode.substr(0, 3);
        sourceCode.remove_prefix(3);
        return bom;
    }
    return {};
}

/**
 * @brief Renders lines into formatted string representation with indentation and intra-line spacing.
 * @param[in] lines Line specifications.
 * @param[in] tokens Annotated token stream.
 * @param[in] options Formatting options.
 * @return Vector of rendered lines.
 */
std::vector<std::string> RenderLines(const std::vector<LineInfo>& lines, const std::vector<Token>& tokens,
                                     const lsp::FormattingOptions& options)
{
    std::vector<std::string> outputLines;
    for (const auto& line : lines)
    {
        if (line.isBlankLine)
        {
            outputLines.push_back("");
            continue;
        }

        std::string lineStr = MakeIndent(line.indentLevel, options);
        for (size_t k = 0; k < line.tokenIndices.size(); ++k)
        {
            size_t tokIdx = line.tokenIndices[k];
            if (k > 0)
            {
                size_t prevTokIdx = line.tokenIndices[k - 1];
                if (NeedsSpaceBetween(tokens, prevTokIdx, tokIdx))
                {
                    lineStr += ' ';
                }
            }
            lineStr += tokens[tokIdx].text;
        }

        if (options.trimTrailingWhitespace.value_or(true))
        {
            while (!lineStr.empty() && (lineStr.back() == ' ' || lineStr.back() == '\t'))
            {
                lineStr.pop_back();
            }
        }
        outputLines.push_back(std::move(lineStr));
    }
    return outputLines;
}

/**
 * @brief Collapses consecutive blank lines and trims trailing empty lines.
 * @param[in,out] lines Rendered lines to collapse in place.
 * @param[in] options Formatting options.
 */
void CollapseAndTrimLines(std::vector<std::string>& lines, const lsp::FormattingOptions& options)
{
    std::vector<std::string> collapsed;
    bool lastWasBlank = false;
    for (auto& l : lines)
    {
        if (l.empty())
        {
            if (!lastWasBlank && !collapsed.empty())
            {
                collapsed.push_back("");
                lastWasBlank = true;
            }
        }
        else
        {
            collapsed.push_back(std::move(l));
            lastWasBlank = false;
        }
    }

    if (options.trimFinalNewlines.value_or(true))
    {
        while (!collapsed.empty() && collapsed.back().empty())
        {
            collapsed.pop_back();
        }
    }
    lines = std::move(collapsed);
}

/**
 * @brief Assembles output text with BOM and final newlines.
 * @param[in] bom Byte order mark string view.
 * @param[in] lines Processed lines.
 * @param[in] options Formatting options.
 * @return Final formatted source text.
 */
std::string AssembleFormattedText(std::string_view bom, const std::vector<std::string>& lines,
                                  const lsp::FormattingOptions& options)
{
    std::string result(bom);
    for (size_t idx = 0; idx < lines.size(); ++idx)
    {
        result += lines[idx];
        if (idx + 1 < lines.size() || options.insertFinalNewline.value_or(true))
        {
            result += '\n';
        }
    }
    return result;
}

/**
 * @brief Splits source text by newline characters.
 * @param[in] text Input text.
 * @param[in] keepTrailingEmpty True to retain trailing empty line after final newline.
 * @return Vector of line strings.
 */
std::vector<std::string> SplitLines(std::string_view text, bool keepTrailingEmpty)
{
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text)
    {
        if (c == '\n')
        {
            lines.push_back(cur);
            cur.clear();
        }
        else if (c != '\r')
        {
            cur += c;
        }
    }
    if (keepTrailingEmpty || !cur.empty())
    {
        lines.push_back(cur);
    }
    return lines;
}

/**
 * @brief Joins a range of lines into a newline-separated string.
 * @param[in] lines Vector of line strings.
 * @param[in] start 0-based start line index.
 * @param[in] end 0-based end line index.
 * @return Joined text string.
 */
std::string JoinLines(const std::vector<std::string>& lines, uint32_t start, uint32_t end)
{
    std::string result;
    for (uint32_t l = start; l <= end && l < lines.size(); ++l)
    {
        result += lines[l];
        if (l < end)
        {
            result += '\n';
        }
    }
    return result;
}
} // namespace

std::string FormatSourceCode(std::string_view sourceCode, const lsp::FormattingOptions& options, BraceStyle braceStyle)
{
    if (sourceCode.empty())
    {
        return "";
    }

    std::string_view bom = ExtractBom(sourceCode);
    auto tokens = Tokenize(sourceCode);
    if (tokens.empty())
    {
        return "";
    }

    auto lines = BuildFormattedLines(tokens, braceStyle);
    auto rendered = RenderLines(lines, tokens, options);
    CollapseAndTrimLines(rendered, options);
    return AssembleFormattedText(bom, rendered, options);
}

std::optional<std::vector<lsp::TextEdit>> FormatDocument(const FormattingRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::vector<lsp::TextEdit>{};
    }

    std::string formatted = FormatSourceCode(request.sourceCode, request.options, request.braceStyle);
    if (formatted == request.sourceCode)
    {
        return std::vector<lsp::TextEdit>{};
    }

    uint32_t lineCount = 0;
    size_t lastLineLen = 0;
    for (size_t i = 0; i < request.sourceCode.size(); ++i)
    {
        if (request.sourceCode[i] == '\n')
        {
            lineCount++;
            lastLineLen = 0;
        }
        else if (request.sourceCode[i] != '\r')
        {
            lastLineLen++;
        }
    }

    lsp::TextEdit edit;
    edit.range.start = lsp::Position{0, 0};
    edit.range.end = lsp::Position{lineCount, static_cast<uint32_t>(lastLineLen)};
    edit.newText = std::move(formatted);

    return std::vector<lsp::TextEdit>{std::move(edit)};
}

std::optional<std::vector<lsp::TextEdit>> FormatRange(const RangeFormattingRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::vector<lsp::TextEdit>{};
    }

    auto origLines = SplitLines(request.sourceCode, true);
    uint32_t totalLines = static_cast<uint32_t>(origLines.size());
    uint32_t startLine = std::min(request.range.start.line, totalLines > 0 ? totalLines - 1 : 0);
    uint32_t endLine = std::min(request.range.end.line, totalLines > 0 ? totalLines - 1 : 0);

    if (startLine == 0 && endLine >= totalLines - 1)
    {
        FormattingRequest fullReq{request.uri, request.sourceCode, request.tree, request.options, request.braceStyle};
        return FormatDocument(fullReq);
    }

    std::string fullFormatted = FormatSourceCode(request.sourceCode, request.options, request.braceStyle);
    if (fullFormatted == request.sourceCode)
    {
        return std::vector<lsp::TextEdit>{};
    }

    auto formattedLines = SplitLines(fullFormatted, false);
    lsp::TextEdit edit;
    edit.range.start = lsp::Position{startLine, 0};
    edit.range.end = lsp::Position{endLine, static_cast<uint32_t>(origLines[endLine].size())};
    edit.newText = JoinLines(formattedLines, startLine, endLine);

    return std::vector<lsp::TextEdit>{std::move(edit)};
}

std::optional<std::vector<lsp::TextEdit>> FormatOnType(const OnTypeFormattingRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    uint32_t targetLine = request.position.line;
    uint32_t startLine = targetLine;

    if (request.ch == "}")
    {
        for (int l = static_cast<int>(targetLine); l >= 0; --l)
        {
            startLine = static_cast<uint32_t>(l);
            if (static_cast<int>(targetLine) - l >= 20)
            {
                break;
            }
        }
    }
    else if (request.ch == "\n" && targetLine > 0)
    {
        startLine = targetLine - 1;
    }

    RangeFormattingRequest rangeReq{
        request.uri,     request.sourceCode,
        request.tree,    lsp::Range{lsp::Position{startLine, 0}, lsp::Position{targetLine, request.position.character}},
        request.options, request.braceStyle};

    return FormatRange(rangeReq);
}
} // namespace angel_lsp::features