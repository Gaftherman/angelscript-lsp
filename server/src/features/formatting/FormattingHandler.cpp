#include "features/formatting/FormattingHandler.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <string_view>
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
            OpenBrace,      // {
            CloseBrace,     // }
            OpenParen,      // (
            CloseParen,     // )
            OpenBracket,    // [
            CloseBracket,   // ]
            Semicolon,      // ;
            Comma,          // ,
            Colon,          // :
            DoubleColon,    // ::
            Question,       // ?
            Dot,            // .
            Arrow,          // ->
            At,             // @
            Operator,       // binary/unary operators: =, ==, +, -, etc.
            Increment,      // ++
            Decrement,      // --
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
        };

        static const std::unordered_set<std::string_view> kKeywords = {
            "and", "auto", "bool", "break", "case", "cast", "class", "const", "continue",
            "default", "do", "double", "else", "enum", "explicit", "external", "false",
            "final", "float", "for", "from", "funcdef", "function", "get", "if", "import",
            "in", "inout", "int", "int8", "int16", "int32", "int64", "interface", "is",
            "mixin", "namespace", "not", "null", "or", "out", "override", "private",
            "property", "protected", "public", "return", "set", "shared", "super",
            "switch", "this", "true", "typedef", "uint", "uint8", "uint16", "uint32",
            "uint64", "void", "while", "xor", "try", "catch", "with"
        };

        static const std::unordered_set<std::string_view> kControlKeywords = {
            "if", "for", "while", "switch", "catch", "with"
        };

        std::vector<Token> Tokenize(std::string_view src)
        {
            std::vector<Token> tokens;
            size_t i = 0;
            uint32_t curLine = 0;
            uint32_t curCol = 0;
            uint32_t newlines = 0;

            while (i < src.size())
            {
                char c = src[i];

                if (c == '\r' || c == '\n')
                {
                    if (c == '\r' && i + 1 < src.size() && src[i + 1] == '\n')
                    {
                        i += 2;
                    }
                    else
                    {
                        i++;
                    }
                    curLine++;
                    curCol = 0;
                    newlines++;
                    continue;
                }

                if (c == ' ' || c == '\t' || c == '\v' || c == '\f')
                {
                    curCol++;
                    i++;
                    continue;
                }

                // Line comment //
                if (c == '/' && i + 1 < src.size() && src[i + 1] == '/')
                {
                    size_t start = i;
                    uint32_t startCol = curCol;
                    while (i < src.size() && src[i] != '\r' && src[i] != '\n')
                    {
                        i++;
                        curCol++;
                    }
                    tokens.push_back({ TokenType::LineComment, std::string(src.substr(start, i - start)), curLine, startCol, newlines });
                    newlines = 0;
                    continue;
                }

                // Block comment /* ... */
                if (c == '/' && i + 1 < src.size() && src[i + 1] == '*')
                {
                    size_t start = i;
                    uint32_t startCol = curCol;
                    i += 2;
                    curCol += 2;
                    while (i < src.size() && !(src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/'))
                    {
                        if (src[i] == '\n')
                        {
                            curLine++;
                            curCol = 0;
                        }
                        else
                        {
                            curCol++;
                        }
                        i++;
                    }
                    if (i < src.size())
                    {
                        i += 2;
                        curCol += 2;
                    }
                    tokens.push_back({ TokenType::BlockComment, std::string(src.substr(start, i - start)), curLine, startCol, newlines });
                    newlines = 0;
                    continue;
                }

                // Preprocessor directive #...
                if (c == '#' && (curCol == 0 || tokens.empty() || newlines > 0))
                {
                    size_t start = i;
                    uint32_t startCol = curCol;
                    while (i < src.size() && src[i] != '\r' && src[i] != '\n')
                    {
                        if (src[i] == '\\' && i + 1 < src.size() && (src[i + 1] == '\r' || src[i + 1] == '\n'))
                        {
                            if (src[i + 1] == '\r' && i + 2 < src.size() && src[i + 2] == '\n')
                            {
                                i += 3;
                            }
                            else
                            {
                                i += 2;
                            }
                            curLine++;
                            curCol = 0;
                            continue;
                        }
                        i++;
                        curCol++;
                    }
                    tokens.push_back({ TokenType::Preprocessor, std::string(src.substr(start, i - start)), curLine, startCol, newlines });
                    newlines = 0;
                    continue;
                }

                // Raw string literal """ ... """
                if (c == '"' && i + 2 < src.size() && src[i + 1] == '"' && src[i + 2] == '"')
                {
                    size_t start = i;
                    uint32_t startCol = curCol;
                    i += 3;
                    curCol += 3;
                    while (i < src.size() && !(src[i] == '"' && i + 2 < src.size() && src[i + 1] == '"' && src[i + 2] == '"'))
                    {
                        if (src[i] == '\n')
                        {
                            curLine++;
                            curCol = 0;
                        }
                        else
                        {
                            curCol++;
                        }
                        i++;
                    }
                    if (i < src.size())
                    {
                        i += 3;
                        curCol += 3;
                    }
                    tokens.push_back({ TokenType::StringLiteral, std::string(src.substr(start, i - start)), curLine, startCol, newlines });
                    newlines = 0;
                    continue;
                }

                // Regular string literal "..."
                if (c == '"')
                {
                    size_t start = i;
                    uint32_t startCol = curCol;
                    i++;
                    curCol++;
                    while (i < src.size() && src[i] != '"')
                    {
                        if (src[i] == '\\' && i + 1 < src.size())
                        {
                            i += 2;
                            curCol += 2;
                            continue;
                        }
                        if (src[i] == '\n' || src[i] == '\r')
                        {
                            break;
                        }
                        i++;
                        curCol++;
                    }
                    if (i < src.size() && src[i] == '"')
                    {
                        i++;
                        curCol++;
                    }
                    tokens.push_back({ TokenType::StringLiteral, std::string(src.substr(start, i - start)), curLine, startCol, newlines });
                    newlines = 0;
                    continue;
                }

                // Character literal '...'
                if (c == '\'')
                {
                    size_t start = i;
                    uint32_t startCol = curCol;
                    i++;
                    curCol++;
                    while (i < src.size() && src[i] != '\'')
                    {
                        if (src[i] == '\\' && i + 1 < src.size())
                        {
                            i += 2;
                            curCol += 2;
                            continue;
                        }
                        if (src[i] == '\n' || src[i] == '\r')
                        {
                            break;
                        }
                        i++;
                        curCol++;
                    }
                    if (i < src.size() && src[i] == '\'')
                    {
                        i++;
                        curCol++;
                    }
                    tokens.push_back({ TokenType::CharacterLiteral, std::string(src.substr(start, i - start)), curLine, startCol, newlines });
                    newlines = 0;
                    continue;
                }

                // Identifiers & Keywords
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
                {
                    size_t start = i;
                    uint32_t startCol = curCol;
                    while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_'))
                    {
                        i++;
                        curCol++;
                    }
                    std::string text(src.substr(start, i - start));
                    TokenType tt = (kKeywords.contains(text)) ? TokenType::Keyword : TokenType::Identifier;
                    tokens.push_back({ tt, std::move(text), curLine, startCol, newlines });
                    newlines = 0;
                    continue;
                }

                // Numbers
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    size_t start = i;
                    uint32_t startCol = curCol;
                    if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'x' || src[i + 1] == 'X'))
                    {
                        i += 2;
                        curCol += 2;
                        while (i < src.size() && std::isxdigit(static_cast<unsigned char>(src[i])))
                        {
                            i++;
                            curCol++;
                        }
                    }
                    else if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'b' || src[i + 1] == 'B'))
                    {
                        i += 2;
                        curCol += 2;
                        while (i < src.size() && (src[i] == '0' || src[i] == '1'))
                        {
                            i++;
                            curCol++;
                        }
                    }
                    else if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'o' || src[i + 1] == 'O'))
                    {
                        i += 2;
                        curCol += 2;
                        while (i < src.size() && (src[i] >= '0' && src[i] <= '7'))
                        {
                            i++;
                            curCol++;
                        }
                    }
                    else
                    {
                        while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i])))
                        {
                            i++;
                            curCol++;
                        }
                        if (i < src.size() && src[i] == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))
                        {
                            i++;
                            curCol++;
                            while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i])))
                            {
                                i++;
                                curCol++;
                            }
                        }
                        if (i < src.size() && (src[i] == 'e' || src[i] == 'E'))
                        {
                            i++;
                            curCol++;
                            if (i < src.size() && (src[i] == '+' || src[i] == '-'))
                            {
                                i++;
                                curCol++;
                            }
                            while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i])))
                            {
                                i++;
                                curCol++;
                            }
                        }
                    }
                    while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_'))
                    {
                        i++;
                        curCol++;
                    }
                    tokens.push_back({ TokenType::Number, std::string(src.substr(start, i - start)), curLine, startCol, newlines });
                    newlines = 0;
                    continue;
                }

                // 4-char operators
                if (i + 3 < src.size())
                {
                    std::string_view op4 = src.substr(i, 4);
                    if (op4 == ">>>=")
                    {
                        tokens.push_back({ TokenType::Operator, std::string(op4), curLine, curCol, newlines });
                        i += 4;
                        curCol += 4;
                        newlines = 0;
                        continue;
                    }
                }

                // 3-char operators
                if (i + 2 < src.size())
                {
                    std::string_view op3 = src.substr(i, 3);
                    if (op3 == "<<=" || op3 == ">>=" || op3 == ">>>" || op3 == "!is")
                    {
                        tokens.push_back({ TokenType::Operator, std::string(op3), curLine, curCol, newlines });
                        i += 3;
                        curCol += 3;
                        newlines = 0;
                        continue;
                    }
                }

                // 2-char operators & symbols
                if (i + 1 < src.size())
                {
                    std::string_view op2 = src.substr(i, 2);
                    if (op2 == "++")
                    {
                        tokens.push_back({ TokenType::Increment, "++", curLine, curCol, newlines });
                        i += 2;
                        curCol += 2;
                        newlines = 0;
                        continue;
                    }
                    if (op2 == "--")
                    {
                        tokens.push_back({ TokenType::Decrement, "--", curLine, curCol, newlines });
                        i += 2;
                        curCol += 2;
                        newlines = 0;
                        continue;
                    }
                    if (op2 == "::")
                    {
                        tokens.push_back({ TokenType::DoubleColon, "::", curLine, curCol, newlines });
                        i += 2;
                        curCol += 2;
                        newlines = 0;
                        continue;
                    }
                    if (op2 == "->")
                    {
                        tokens.push_back({ TokenType::Arrow, "->", curLine, curCol, newlines });
                        i += 2;
                        curCol += 2;
                        newlines = 0;
                        continue;
                    }
                    if (op2 == "==" || op2 == "!=" || op2 == "<=" || op2 == ">=" ||
                        op2 == "&&" || op2 == "||" || op2 == "+=" || op2 == "-=" ||
                        op2 == "*=" || op2 == "/=" || op2 == "%=" || op2 == "&=" ||
                        op2 == "|=" || op2 == "^=" || op2 == "<<" || op2 == ">>" ||
                        op2 == "^^")
                    {
                        tokens.push_back({ TokenType::Operator, std::string(op2), curLine, curCol, newlines });
                        i += 2;
                        curCol += 2;
                        newlines = 0;
                        continue;
                    }
                }

                // Single-char tokens
                TokenType tt = TokenType::Operator;
                switch (c)
                {
                case '{': tt = TokenType::OpenBrace; break;
                case '}': tt = TokenType::CloseBrace; break;
                case '(': tt = TokenType::OpenParen; break;
                case ')': tt = TokenType::CloseParen; break;
                case '[': tt = TokenType::OpenBracket; break;
                case ']': tt = TokenType::CloseBracket; break;
                case ';': tt = TokenType::Semicolon; break;
                case ',': tt = TokenType::Comma; break;
                case ':': tt = TokenType::Colon; break;
                case '?': tt = TokenType::Question; break;
                case '.': tt = TokenType::Dot; break;
                case '@': tt = TokenType::At; break;
                default: tt = TokenType::Operator; break;
                }

                tokens.push_back({ tt, std::string(1, c), curLine, curCol, newlines });
                i++;
                curCol++;
                newlines = 0;
            }

            // Split any '>>' or '>>>' operator tokens into individual '>' tokens so nested templates are cleanly recognized
            std::vector<Token> splitTokens;
            splitTokens.reserve(tokens.size());
            for (const auto &tok : tokens)
            {
                if (tok.type == TokenType::Operator && tok.text == ">>")
                {
                    splitTokens.push_back({ TokenType::Operator, ">", tok.line, tok.column, tok.newlinesBefore });
                    splitTokens.push_back({ TokenType::Operator, ">", tok.line, tok.column + 1, 0 });
                }
                else if (tok.type == TokenType::Operator && tok.text == ">>>")
                {
                    splitTokens.push_back({ TokenType::Operator, ">", tok.line, tok.column, tok.newlinesBefore });
                    splitTokens.push_back({ TokenType::Operator, ">", tok.line, tok.column + 1, 0 });
                    splitTokens.push_back({ TokenType::Operator, ">", tok.line, tok.column + 2, 0 });
                }
                else
                {
                    splitTokens.push_back(tok);
                }
            }
            tokens = std::move(splitTokens);

            // Identify template '<' and '>'
            for (size_t t = 0; t < tokens.size(); ++t)
            {
                if (tokens[t].text == "<" && t > 0)
                {
                    const auto &prev = tokens[t - 1];
                    if (prev.type == TokenType::Identifier || prev.text == "cast" || prev.isTemplateCloser)
                    {
                        int depth = 1;
                        size_t matchIdx = t + 1;
                        bool valid = true;
                        std::vector<size_t> closerIndices;

                        while (matchIdx < tokens.size() && depth > 0)
                        {
                            if (tokens[matchIdx].type == TokenType::Semicolon ||
                                tokens[matchIdx].type == TokenType::OpenBrace ||
                                tokens[matchIdx].type == TokenType::CloseBrace)
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
                }
            }

            return tokens;
        }

        bool IsUnary(const std::vector<Token> &tokens, size_t idx)
        {
            if (tokens[idx].text != "+" && tokens[idx].text != "-")
            {
                return false;
            }
            if (idx == 0)
            {
                return true;
            }
            const auto &prev = tokens[idx - 1];
            if (prev.type == TokenType::OpenParen || prev.type == TokenType::OpenBracket ||
                prev.type == TokenType::Comma || prev.type == TokenType::Semicolon ||
                prev.type == TokenType::Question || prev.type == TokenType::Colon ||
                prev.type == TokenType::Operator || prev.type == TokenType::Increment ||
                prev.type == TokenType::Decrement)
            {
                return true;
            }
            if (prev.type == TokenType::Keyword)
            {
                if (prev.text == "return" || prev.text == "case" || prev.text == "throw" || prev.text == "is")
                {
                    return true;
                }
            }
            return false;
        }

        bool IsAccessSpecifierOrLabelColon(const std::vector<Token> &tokens, size_t idx)
        {
            if (tokens[idx].type != TokenType::Colon)
            {
                return false;
            }
            if (idx == 0)
            {
                return false;
            }
            for (int k = static_cast<int>(idx) - 1; k >= 0; --k)
            {
                if (tokens[k].type == TokenType::Semicolon || tokens[k].type == TokenType::OpenBrace || tokens[k].type == TokenType::CloseBrace)
                {
                    break;
                }
                if (tokens[k].type == TokenType::Keyword)
                {
                    if (tokens[k].text == "case" || tokens[k].text == "default" ||
                        tokens[k].text == "public" || tokens[k].text == "private" ||
                        tokens[k].text == "protected")
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        bool NeedsSpaceBetween(const std::vector<Token> &tokens, size_t prevIdx, size_t currIdx)
        {
            const auto &prev = tokens[prevIdx];
            const auto &curr = tokens[currIdx];

            // Never space before punctuation
            if (curr.type == TokenType::Comma || curr.type == TokenType::Semicolon ||
                curr.type == TokenType::CloseParen || curr.type == TokenType::CloseBracket ||
                curr.type == TokenType::Dot || curr.type == TokenType::DoubleColon ||
                curr.type == TokenType::Arrow)
            {
                return false;
            }

            // Never space after punctuation
            if (prev.type == TokenType::OpenParen || prev.type == TokenType::OpenBracket ||
                prev.type == TokenType::Dot || prev.type == TokenType::DoubleColon ||
                prev.type == TokenType::Arrow)
            {
                return false;
            }

            // Postfix ++ / --
            if (curr.type == TokenType::Increment || curr.type == TokenType::Decrement)
            {
                if (prev.type == TokenType::Identifier || prev.type == TokenType::CloseBracket || prev.type == TokenType::CloseParen)
                {
                    return false;
                }
            }

            // Prefix ++ / --
            if (prev.type == TokenType::Increment || prev.type == TokenType::Decrement)
            {
                return false;
            }

            // Unary ! and ~
            if (prev.text == "!" || prev.text == "~")
            {
                return false;
            }

            // No space between unary operator and its operand
            if (IsUnary(tokens, prevIdx))
            {
                return false;
            }

            // Space before unary operator if preceded by binary operator, keyword, comma
            if (IsUnary(tokens, currIdx))
            {
                if (prev.type == TokenType::OpenParen || prev.type == TokenType::OpenBracket)
                {
                    return false;
                }
                return true;
            }

            // At symbol @ (handles)
            if (curr.type == TokenType::At)
            {
                return false;
            }
            if (prev.type == TokenType::At)
            {
                if (curr.type == TokenType::Identifier || curr.type == TokenType::Keyword)
                {
                    return true;
                }
                return false;
            }

            // Template brackets
            if (curr.isTemplateOpener || prev.isTemplateOpener)
            {
                return false;
            }
            if (curr.isTemplateCloser)
            {
                return false;
            }
            if (prev.isTemplateCloser)
            {
                if (curr.type == TokenType::Identifier || curr.type == TokenType::Keyword ||
                    curr.type == TokenType::OpenParen || curr.type == TokenType::Operator)
                {
                    return true;
                }
                return false;
            }

            // Function calls / declarations: no space before '(' unless control keyword
            if (curr.type == TokenType::OpenParen)
            {
                if (prev.type == TokenType::Keyword && kControlKeywords.contains(prev.text))
                {
                    return true;
                }
                if (prev.type == TokenType::Identifier || prev.text == "super" || prev.text == "this" || prev.text == "cast")
                {
                    return false;
                }
            }

            // Array index: no space before '[' if after identifier/close bracket/paren
            if (curr.type == TokenType::OpenBracket)
            {
                if (prev.type == TokenType::Identifier || prev.type == TokenType::CloseBracket || prev.type == TokenType::CloseParen)
                {
                    return false;
                }
            }

            // Colon
            if (curr.type == TokenType::Colon)
            {
                if (IsAccessSpecifierOrLabelColon(tokens, currIdx))
                {
                    return false;
                }
                return true;
            }

            // Space after comma
            if (prev.type == TokenType::Comma)
            {
                return true;
            }

            // Space after semicolon (in for loop)
            if (prev.type == TokenType::Semicolon)
            {
                return true;
            }

            // Binary operators
            if (curr.type == TokenType::Operator && !curr.isTemplateOpener && !curr.isTemplateCloser)
            {
                return true;
            }
            if (prev.type == TokenType::Operator && !prev.isTemplateOpener && !prev.isTemplateCloser)
            {
                return true;
            }

            // Ternary question
            if (curr.type == TokenType::Question || prev.type == TokenType::Question)
            {
                return true;
            }

            // Colon after ternary/inheritance
            if (prev.type == TokenType::Colon)
            {
                return true;
            }

            // Space between keywords, identifiers, numbers, literals
            if (prev.type == TokenType::Keyword || curr.type == TokenType::Keyword)
            {
                return true;
            }
            if (prev.type == TokenType::Identifier && (curr.type == TokenType::Identifier || curr.type == TokenType::Number || curr.type == TokenType::StringLiteral))
            {
                return true;
            }
            if (prev.type == TokenType::CloseParen && (curr.type == TokenType::Identifier || curr.type == TokenType::Keyword || curr.type == TokenType::OpenBrace))
            {
                return true;
            }
            if (prev.type == TokenType::CloseBracket && (curr.type == TokenType::Identifier || curr.type == TokenType::Keyword))
            {
                return true;
            }
            if (curr.type == TokenType::LineComment || curr.type == TokenType::BlockComment)
            {
                return true;
            }

            return false;
        }

        std::string MakeIndent(int level, const lsp::FormattingOptions &options)
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
            else
            {
                return std::string(static_cast<size_t>(level), '\t');
            }
        }

        enum class ScopeKind
        {
            Generic,
            Switch,
            Enum,
            Class
        };

        struct LineInfo
        {
            int indentLevel = 0;
            bool isPreprocessor = false;
            bool isBlankLine = false;
            std::vector<size_t> tokenIndices;
        };
    }

    std::string FormatSourceCode(std::string_view sourceCode, const lsp::FormattingOptions &options)
    {
        if (sourceCode.empty())
        {
            return "";
        }

        auto tokens = Tokenize(sourceCode);
        if (tokens.empty())
        {
            return "";
        }

        std::vector<LineInfo> lines;
        LineInfo currentLine;
        int braceLevel = 0;
        int parenDepth = 0;
        int bracketDepth = 0;

        std::vector<ScopeKind> scopeStack;
        ScopeKind pendingScope = ScopeKind::Generic;
        bool insideCaseBody = false;

        auto flushCurrentLine = [&]()
        {
            if (!currentLine.tokenIndices.empty() || currentLine.isBlankLine)
            {
                lines.push_back(std::move(currentLine));
                currentLine = LineInfo{};
            }
        };

        for (size_t i = 0; i < tokens.size(); ++i)
        {
            const auto &tok = tokens[i];

            // Preserve intentional blank lines
            if (tok.newlinesBefore >= 2 && !lines.empty() && !lines.back().isBlankLine)
            {
                flushCurrentLine();
                LineInfo blank;
                blank.isBlankLine = true;
                lines.push_back(std::move(blank));
            }

            if (tok.type == TokenType::Keyword)
            {
                if (tok.text == "switch")
                {
                    pendingScope = ScopeKind::Switch;
                }
                else if (tok.text == "enum")
                {
                    pendingScope = ScopeKind::Enum;
                }
                else if (tok.text == "class" || tok.text == "interface")
                {
                    pendingScope = ScopeKind::Class;
                }
            }

            if (tok.type == TokenType::OpenParen)
            {
                parenDepth++;
            }
            else if (tok.type == TokenType::CloseParen)
            {
                if (parenDepth > 0) parenDepth--;
            }
            else if (tok.type == TokenType::OpenBracket)
            {
                bracketDepth++;
            }
            else if (tok.type == TokenType::CloseBracket)
            {
                if (bracketDepth > 0) bracketDepth--;
            }

            // Preprocessor
            if (tok.type == TokenType::Preprocessor)
            {
                flushCurrentLine();
                LineInfo prep;
                prep.isPreprocessor = true;
                prep.indentLevel = 0;
                prep.tokenIndices.push_back(i);
                lines.push_back(std::move(prep));
                continue;
            }

            // Open brace {
            if (tok.type == TokenType::OpenBrace)
            {
                flushCurrentLine();
                LineInfo braceLine;
                braceLine.indentLevel = braceLevel;
                braceLine.tokenIndices.push_back(i);
                lines.push_back(std::move(braceLine));

                scopeStack.push_back(pendingScope);
                pendingScope = ScopeKind::Generic;
                insideCaseBody = false;

                braceLevel++;
                continue;
            }

            // Close brace }
            if (tok.type == TokenType::CloseBrace)
            {
                flushCurrentLine();
                braceLevel = std::max(0, braceLevel - 1);
                if (!scopeStack.empty())
                {
                    scopeStack.pop_back();
                }
                insideCaseBody = false;

                LineInfo braceLine;
                braceLine.indentLevel = braceLevel;
                braceLine.tokenIndices.push_back(i);

                // If followed immediately by ';' (e.g. struct/class/enum definition end '};')
                if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Semicolon)
                {
                    i++;
                    braceLine.tokenIndices.push_back(i);
                }
                lines.push_back(std::move(braceLine));
                continue;
            }

            // Control keyword 'else'
            if (tok.type == TokenType::Keyword && tok.text == "else")
            {
                flushCurrentLine();
                currentLine.indentLevel = braceLevel;
                currentLine.tokenIndices.push_back(i);
                continue;
            }

            // Case / default label
            if (tok.type == TokenType::Keyword && (tok.text == "case" || tok.text == "default"))
            {
                flushCurrentLine();
                insideCaseBody = false;
                currentLine.indentLevel = braceLevel;
                currentLine.tokenIndices.push_back(i);
                continue;
            }

            // Access specifiers
            if (tok.type == TokenType::Keyword && (tok.text == "public" || tok.text == "private" || tok.text == "protected"))
            {
                if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Colon)
                {
                    flushCurrentLine();
                    currentLine.indentLevel = std::max(0, braceLevel - 1);
                    currentLine.tokenIndices.push_back(i);
                    i++;
                    currentLine.tokenIndices.push_back(i);
                    flushCurrentLine();
                    continue;
                }
            }

            // First token on line
            if (currentLine.tokenIndices.empty())
            {
                if (insideCaseBody && !scopeStack.empty() && scopeStack.back() == ScopeKind::Switch)
                {
                    currentLine.indentLevel = braceLevel + 1;
                }
                else
                {
                    currentLine.indentLevel = braceLevel;
                }
            }

            currentLine.tokenIndices.push_back(i);

            // Semicolon outside paren/bracket ends statement
            if (tok.type == TokenType::Semicolon && parenDepth == 0 && bracketDepth == 0)
            {
                // Check if next token is trailing comment on same line
                if (i + 1 < tokens.size() && tokens[i + 1].newlinesBefore == 0 &&
                    (tokens[i + 1].type == TokenType::LineComment || tokens[i + 1].type == TokenType::BlockComment))
                {
                    i++;
                    currentLine.tokenIndices.push_back(i);
                }
                flushCurrentLine();
                continue;
            }

            // Comma in enum body breaks line
            if (tok.type == TokenType::Comma && parenDepth == 0 && bracketDepth == 0 &&
                !scopeStack.empty() && scopeStack.back() == ScopeKind::Enum)
            {
                // Check if next token is trailing comment on same line
                if (i + 1 < tokens.size() && tokens[i + 1].newlinesBefore == 0 &&
                    (tokens[i + 1].type == TokenType::LineComment || tokens[i + 1].type == TokenType::BlockComment))
                {
                    i++;
                    currentLine.tokenIndices.push_back(i);
                }
                flushCurrentLine();
                continue;
            }

            // Line comment ends line
            if (tok.type == TokenType::LineComment)
            {
                flushCurrentLine();
                continue;
            }

            // Case/default label colon ends label line
            if (tok.type == TokenType::Colon && IsAccessSpecifierOrLabelColon(tokens, i))
            {
                flushCurrentLine();
                if (!scopeStack.empty() && scopeStack.back() == ScopeKind::Switch)
                {
                    insideCaseBody = true;
                }
                continue;
            }
        }

        flushCurrentLine();

        // Render lines
        std::vector<std::string> outputLines;
        for (const auto &line : lines)
        {
            if (line.isBlankLine)
            {
                outputLines.push_back("");
                continue;
            }

            std::string lineStr;
            if (!line.isPreprocessor)
            {
                lineStr = MakeIndent(line.indentLevel, options);
            }

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

            // Trim trailing whitespace if requested
            if (options.trimTrailingWhitespace.value_or(true))
            {
                while (!lineStr.empty() && (lineStr.back() == ' ' || lineStr.back() == '\t'))
                {
                    lineStr.pop_back();
                }
            }

            outputLines.push_back(std::move(lineStr));
        }

        // Collapse consecutive blank lines > 1
        std::vector<std::string> collapsedLines;
        bool lastWasBlank = false;
        for (auto &l : outputLines)
        {
            if (l.empty())
            {
                if (!lastWasBlank && !collapsedLines.empty())
                {
                    collapsedLines.push_back("");
                    lastWasBlank = true;
                }
            }
            else
            {
                collapsedLines.push_back(std::move(l));
                lastWasBlank = false;
            }
        }

        // Trim final newlines / blank lines if requested
        if (options.trimFinalNewlines.value_or(true))
        {
            while (!collapsedLines.empty() && collapsedLines.back().empty())
            {
                collapsedLines.pop_back();
            }
        }

        // Build result text
        std::string result;
        for (size_t idx = 0; idx < collapsedLines.size(); ++idx)
        {
            result += collapsedLines[idx];
            if (idx + 1 < collapsedLines.size() || options.insertFinalNewline.value_or(true))
            {
                result += '\n';
            }
        }

        return result;
    }

    std::optional<std::vector<lsp::TextEdit>> FormatDocument(const FormattingRequest &request)
    {
        if (request.sourceCode.empty())
        {
            return std::vector<lsp::TextEdit>{};
        }

        std::string formatted = FormatSourceCode(request.sourceCode, request.options);
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
        edit.range.start = lsp::Position{ 0, 0 };
        edit.range.end = lsp::Position{ lineCount, lastLineLen };
        edit.newText = std::move(formatted);

        return std::vector<lsp::TextEdit>{ std::move(edit) };
    }

    std::optional<std::vector<lsp::TextEdit>> FormatRange(const RangeFormattingRequest &request)
    {
        if (request.sourceCode.empty())
        {
            return std::vector<lsp::TextEdit>{};
        }

        // Split sourceCode into lines
        std::vector<std::string> origLines;
        {
            std::string cur;
            for (char c : request.sourceCode)
            {
                if (c == '\n')
                {
                    origLines.push_back(cur);
                    cur.clear();
                }
                else if (c != '\r')
                {
                    cur += c;
                }
            }
            origLines.push_back(cur);
        }

        uint32_t totalLines = static_cast<uint32_t>(origLines.size());
        uint32_t startLine = std::min(request.range.start.line, totalLines > 0 ? totalLines - 1 : 0);
        uint32_t endLine = std::min(request.range.end.line, totalLines > 0 ? totalLines - 1 : 0);

        if (startLine == 0 && endLine >= totalLines - 1)
        {
            FormattingRequest fullReq{ request.uri, request.sourceCode, request.tree, request.options };
            return FormatDocument(fullReq);
        }

        // Format document
        std::string fullFormatted = FormatSourceCode(request.sourceCode, request.options);
        if (fullFormatted == request.sourceCode)
        {
            return std::vector<lsp::TextEdit>{};
        }

        std::vector<std::string> formattedLines;
        {
            std::string cur;
            for (char c : fullFormatted)
            {
                if (c == '\n')
                {
                    formattedLines.push_back(cur);
                    cur.clear();
                }
                else if (c != '\r')
                {
                    cur += c;
                }
            }
            if (!cur.empty())
            {
                formattedLines.push_back(cur);
            }
        }

        // Compute edit covering startLine to endLine
        lsp::TextEdit edit;
        edit.range.start = lsp::Position{ startLine, 0 };
        edit.range.end = lsp::Position{ endLine, static_cast<uint32_t>(origLines[endLine].size()) };

        std::string rangeFormattedText;
        for (uint32_t l = startLine; l <= endLine && l < formattedLines.size(); ++l)
        {
            rangeFormattedText += formattedLines[l];
            if (l < endLine)
            {
                rangeFormattedText += '\n';
            }
        }

        edit.newText = std::move(rangeFormattedText);
        return std::vector<lsp::TextEdit>{ std::move(edit) };
    }
}