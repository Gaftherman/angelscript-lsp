#include "features/formatting/FormattingLexer.h"
#include "parser/Keywords.h"
#include <cctype>
#include <unordered_set>

namespace angel_lsp::features
{
namespace
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

struct ScanState
{
    explicit ScanState(std::string_view source) : src(source)
    {
    }

    std::string_view src;
    size_t i = 0;
    uint32_t curLine = 0;
    uint32_t curCol = 0;
    uint32_t newlines = 0;
    std::vector<Token> tokens;
};

bool TryConsumeWhitespace(ScanState& state)
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

bool TryConsumeLineComment(ScanState& state)
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

bool TryConsumeBlockComment(ScanState& state)
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

bool TryConsumePreprocessor(ScanState& state)
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
                state.i +=
                    (state.src[state.i + 1] == '\r' && state.i + 2 < state.src.size() && state.src[state.i + 2] == '\n')
                        ? 3
                        : 2;
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

bool TryConsumeRawString(ScanState& state)
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

bool TryConsumeQuotedLiteral(ScanState& state, char quoteChar, TokenType tokenType)
{
    if (state.src[state.i] != quoteChar)
        return false;
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
            break;
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

bool TryConsumeIdentifierOrNonAscii(ScanState& state)
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

void ConsumeExponent(ScanState& state)
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

void ConsumeDecimalNumber(ScanState& state)
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

bool TryConsumeNumber(ScanState& state)
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
    state.tokens.push_back({TokenType::Number, std::string(state.src.substr(start, state.i - start)), state.curLine,
                            startCol, state.newlines});
    state.newlines = 0;
    return true;
}

TokenType ClassifyTwoCharOperator(std::string_view op2)
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

bool TryConsumeMultiCharOperator(ScanState& state)
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

TokenType ClassifySingleChar(char c)
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

void ConsumeSingleCharToken(ScanState& state)
{
    char c = state.src[state.i];
    TokenType tt = ClassifySingleChar(c);
    state.tokens.push_back({tt, std::string(1, c), state.curLine, state.curCol, state.newlines});
    state.i++;
    state.curCol++;
    state.newlines = 0;
}
} // namespace

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
} // namespace angel_lsp::features
