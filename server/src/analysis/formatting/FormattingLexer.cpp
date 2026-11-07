#include "analysis/formatting/FormattingLexer.h"

#include "parser/Keywords.h"

#include <cctype>
#include <unordered_set>

namespace angel_lsp::analysis::formatting
{
static const std::unordered_set<std::string_view> kKeywords = []
{
    std::unordered_set<std::string_view> all;
    all.insert(parser::keywords::k_reserved.begin(), parser::keywords::k_reserved.end());
    all.insert(parser::keywords::k_contextual.begin(), parser::keywords::k_contextual.end());
    return all;
}();

static constexpr std::string_view kTwoCharOperators[] = {
    "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "^^"};

static bool TryConsumeWhitespace(ScanState& state)
{
    char c = state.src[state.i];
    if (c == '\r' || c == '\n')
    {
        if (c == '\r' && state.i + 1 < state.src.size() && state.src[state.i + 1] == '\n')
            state.i += 2;
        else
            state.i++;
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

static bool TryConsumeIdentifierOrNonAscii(ScanState& state)
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
        state.tokens.push_back(
            {TokenType::Identifier, state.src.substr(start, state.i - start), state.curLine, startCol, state.newlines});
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
        std::string_view text(state.src.substr(start, state.i - start));
        TokenType tt = (kKeywords.contains(text)) ? TokenType::Keyword : TokenType::Identifier;
        state.tokens.push_back({tt, std::move(text), state.curLine, startCol, state.newlines});
        state.newlines = 0;
        return true;
    }
    return false;
}

static void ConsumeExponent(ScanState& state)
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

static void ConsumeDecimalNumber(ScanState& state)
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
    ConsumeExponent(state);
}

static bool TryConsumeNumber(ScanState& state)
{
    char c = state.src[state.i];
    if (!std::isdigit(static_cast<unsigned char>(c)))
        return false;
    size_t start = state.i;
    uint32_t startCol = state.curCol;
    if (c == '0' && state.i + 1 < state.src.size())
    {
        char p = static_cast<char>(std::tolower(static_cast<unsigned char>(state.src[state.i + 1])));
        if (p == 'x' || p == 'b' || p == 'o')
        {
            state.i += 2;
            state.curCol += 2;
            while (state.i < state.src.size() && (std::isxdigit(static_cast<unsigned char>(state.src[state.i])) ||
                                                  state.src[state.i] == '_' || state.src[state.i] == '.'))
            {
                state.i++;
                state.curCol++;
            }
        }
        else
            ConsumeDecimalNumber(state);
    }
    else
        ConsumeDecimalNumber(state);

    while (state.i < state.src.size() &&
           (std::isalnum(static_cast<unsigned char>(state.src[state.i])) || state.src[state.i] == '_'))
    {
        state.i++;
        state.curCol++;
    }
    state.tokens.push_back(
        {TokenType::Number, state.src.substr(start, state.i - start), state.curLine, startCol, state.newlines});
    state.newlines = 0;
    return true;
}

static TokenType ClassifyTwoCharOperator(std::string_view op2)
{
    if (op2 == "++")
        return TokenType::Increment;
    if (op2 == "--")
        return TokenType::Decrement;
    if (op2 == "::")
        return TokenType::DoubleColon;
    for (std::string_view candidate : kTwoCharOperators)
    {
        if (op2 == candidate)
            return TokenType::Operator;
    }
    return TokenType::EndOfFile;
}

static bool TryConsumeMultiCharOperator(ScanState& state)
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
        const bool isWordOp = (op3 == "!is");
        const bool boundary =
            !isWordOp || state.i + 3 >= state.src.size() ||
            (std::isalnum(static_cast<unsigned char>(state.src[state.i + 3])) == 0 && state.src[state.i + 3] != '_');
        if ((op3 == "<<=" || op3 == ">>=" || op3 == ">>>" || isWordOp) && boundary)
        {
            state.tokens.push_back({TokenType::Operator, op3, state.curLine, state.curCol, state.newlines});
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
            state.tokens.push_back({tt, state.src.substr(state.i, 2), state.curLine, state.curCol, state.newlines});
            state.i += 2;
            state.curCol += 2;
            state.newlines = 0;
            return true;
        }
    }
    return false;
}

static TokenType ClassifySingleChar(char c)
{
    switch (c)
    {
    case '{':
        return TokenType::OpenBrace;
    case '}':
        return TokenType::CloseBrace;
    case '(':
        return TokenType::OpenParen;
    case ')':
        return TokenType::CloseParen;
    case '[':
        return TokenType::OpenBracket;
    case ']':
        return TokenType::CloseBracket;
    case ';':
        return TokenType::Semicolon;
    case ',':
        return TokenType::Comma;
    case ':':
        return TokenType::Colon;
    case '?':
        return TokenType::Question;
    case '.':
        return TokenType::Dot;
    case '@':
        return TokenType::At;
    default:
        return TokenType::Operator;
    }
}

static void ConsumeSingleCharToken(ScanState& state)
{
    char c = state.src[state.i];
    TokenType tt = ClassifySingleChar(c);
    state.tokens.push_back({tt, state.src.substr(state.i, 1), state.curLine, state.curCol, state.newlines});
    state.i++;
    state.curCol++;
    state.newlines = 0;
}

std::vector<Token> ScanRawTokens(std::string_view src)
{
    ScanState state{src};
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
    return state.tokens;
}
} // namespace angel_lsp::analysis::formatting
