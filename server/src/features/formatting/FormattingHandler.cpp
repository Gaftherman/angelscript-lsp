#include "features/formatting/FormattingHandler.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <string_view>
#include <vector>
#include "parser/Keywords.h"

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
                    const bool stringClosed = i < src.size() && src[i] == '"';
                    if (stringClosed)
                    {
                        i++;
                        curCol++;
                    }
                    Token stringTok{ TokenType::StringLiteral, std::string(src.substr(start, i - start)), curLine, startCol, newlines };
                    stringTok.isUnterminated = !stringClosed;
                    tokens.push_back(std::move(stringTok));
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
                    const bool charClosed = i < src.size() && src[i] == '\'';
                    if (charClosed)
                    {
                        i++;
                        curCol++;
                    }
                    Token charTok{ TokenType::CharacterLiteral, std::string(src.substr(start, i - start)), curLine, startCol, newlines };
                    charTok.isUnterminated = !charClosed;
                    tokens.push_back(std::move(charTok));
                    newlines = 0;
                    continue;
                }

                // A run of non-ASCII bytes, kept whole.
                //
                // Nothing below this recognises one, so each byte of a UTF-8 sequence used to come
                // out as its own token and the renderer put spaces between them - which is how a
                // file that compiled before formatting stopped compiling after. Held together as
                // one token it survives untouched. The formatter has no business inside a
                // multi-byte sequence: AngelScript's own grammar is ASCII, so anything here is
                // inside something the tokenizer already failed to claim.
                if (static_cast<unsigned char>(c) >= 0x80)
                {
                    size_t start = i;
                    uint32_t startCol = curCol;
                    while (i < src.size() && static_cast<unsigned char>(src[i]) >= 0x80)
                    {
                        i++;
                        curCol++;
                    }
                    tokens.push_back({ TokenType::Identifier, std::string(src.substr(start, i - start)), curLine, startCol, newlines });
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

                    // `!is` is the handle-inequality operator, and it is the only operator here
                    // spelled with letters - so it is the only one that can swallow the front of
                    // an identifier. `!isdigit(x)` matched it and came out as `!is digit(x)`,
                    // which is not the same program and does not compile. Twenty-eight files in
                    // the 1061-script corpus were being rewritten this way. A word boundary after
                    // it is what tells the operator from the call.
                    const bool op3IsWordOperator = op3 == "!is";
                    const bool op3EndsAtWordBoundary =
                        !op3IsWordOperator || i + 3 >= src.size() ||
                        (std::isalnum(static_cast<unsigned char>(src[i + 3])) == 0 && src[i + 3] != '_');

                    if ((op3 == "<<=" || op3 == ">>=" || op3 == ">>>" || op3IsWordOperator) &&
                        op3EndsAtWordBoundary)
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

            // An omitted initializer-list element, which is legal and takes the type's default:
            // `{0, 1, , 4}`. The hole is a gap between two separators and has no token of its
            // own, so without this it closes up into `{0, 1,, 4}` - still the same four elements
            // to the compiler, and invisible to whoever has to read it next.
            if (curr.type == TokenType::Comma &&
                (prev.type == TokenType::Comma || prev.type == TokenType::OpenBrace))
            {
                return true;
            }

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

            // Braces only share a line when they carry a value, and the padding then follows what
            // the brace holds: `{ Log(a); }` for a lambda body, `{1, 2}` for a list. Decided while
            // the braces were classified, since that is the pass that knows which is which.
            if (prev.type == TokenType::OpenBrace)
            {
                return prev.isPaddedBrace;
            }
            if (curr.type == TokenType::CloseBrace)
            {
                return curr.isPaddedBrace;
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
                // `function` opens a lambda's parameter list the way an identifier opens a call's,
                // so it takes no space either - `function(int a)`, not `function (int a)`.
                if (prev.type == TokenType::Identifier || prev.text == "super" ||
                    prev.text == "this" || prev.text == "cast" || prev.text == "function")
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

            // Last guard, and the only one here that is not about style: two tokens that would
            // re-read as one token must never be written next to each other. `A = 1 B = 2` came
            // back as `A = 1B = 2` - a number welded to an identifier, four tokens turned into
            // three, in a file the formatter had been handed to tidy.
            //
            // The rule above covers an identifier followed by a word; a number followed by one
            // fell through to `return false`. That input is a syntax error, which is exactly when
            // it matters: the formatter runs on whatever is in the editor, including half-typed
            // code, and losing a character there is not a formatting choice.
            if ((prev.type == TokenType::Number || prev.type == TokenType::StringLiteral) &&
                (curr.type == TokenType::Identifier || curr.type == TokenType::Number ||
                 curr.type == TokenType::StringLiteral))
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
            Class,
            Value    ///< An initializer list or a lambda body - a brace that produces a value.
        };

        /**
         * @brief One open brace, and the paren/bracket nesting the source was at when it opened.
         *
         * The depths are what separates a lambda's body from an `if` block written inside it.
         * `f(function() { if (c) { g(); } })` has both braces at `parenDepth == 1`, so an absolute
         * test - "inside parentheses means it is a value" - calls the `if` block a value too and
         * runs its body onto one line. Measured against the brace that encloses it, the lambda body
         * opens *deeper* than its enclosing scope and the `if` block opens at the same depth, which
         * is the distinction that actually holds.
         */
        struct ScopeEntry
        {
            ScopeKind kind = ScopeKind::Generic;
            int parenDepthAtOpen = 0;
            int bracketDepthAtOpen = 0;
            bool paddedBrace = false;  ///< Carried to the closing brace, which shares the answer.
        };

        struct LineInfo
        {
            int indentLevel = 0;
            bool isPreprocessor = false;
            bool isBlankLine = false;
            std::vector<size_t> tokenIndices;
        };
    }

    std::string FormatSourceCode(std::string_view sourceCode, const lsp::FormattingOptions &options,
                                 BraceStyle braceStyle)
    {
        if (sourceCode.empty())
        {
            return "";
        }

        // A UTF-8 BOM is held aside and put back byte for byte. The compiler accepts one and so
        // does the grammar, so it must survive formatting - and it is not a token: left in the
        // stream it becomes the first "identifier" on line one, which is a different file.
        std::string_view bom;
        if (sourceCode.size() >= 3 && static_cast<unsigned char>(sourceCode[0]) == 0xEF &&
            static_cast<unsigned char>(sourceCode[1]) == 0xBB &&
            static_cast<unsigned char>(sourceCode[2]) == 0xBF)
        {
            bom = sourceCode.substr(0, 3);
            sourceCode.remove_prefix(3);
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

        std::vector<ScopeEntry> scopeStack;
        ScopeKind pendingScope = ScopeKind::Generic;
        bool insideCaseBody = false;

        // Set by `=` or `return` at the nesting the enclosing brace opened at, cleared at the end
        // of the statement. It is what makes `array<int> a = {1, 2};` a value brace while
        // `if (x = 5) {}` and `for (int i = 0; ...) {}` keep theirs as blocks - in those two the
        // `=` sits inside parentheses, deeper than the scope, so it never arms this.
        bool inValueContext = false;

        auto currentScopeKind = [&]() -> ScopeKind
        {
            return scopeStack.empty() ? ScopeKind::Generic : scopeStack.back().kind;
        };

        auto flushCurrentLine = [&]()
        {
            if (!currentLine.tokenIndices.empty() || currentLine.isBlankLine)
            {
                lines.push_back(std::move(currentLine));
                currentLine = LineInfo{};
            }
        };

        // The indent a fresh line takes, which a case body pushes one level deeper.
        auto beginLineIfEmpty = [&]()
        {
            if (!currentLine.tokenIndices.empty())
            {
                return;
            }
            const bool inCaseBody = insideCaseBody && currentScopeKind() == ScopeKind::Switch;
            currentLine.indentLevel = inCaseBody ? braceLevel + 1 : braceLevel;
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

            const int baseParen = scopeStack.empty() ? 0 : scopeStack.back().parenDepthAtOpen;
            const int baseBracket = scopeStack.empty() ? 0 : scopeStack.back().bracketDepthAtOpen;
            const bool atScopeNesting = parenDepth == baseParen && bracketDepth == baseBracket;

            // Arm and disarm the value context. A top-level comma separates two declarators
            // (`int[] a = {1}, b = {2};`), so it ends the first one's value context - but a comma
            // *inside* a list separates its elements and must leave it alone.
            if (atScopeNesting && currentScopeKind() != ScopeKind::Value)
            {
                if ((tok.type == TokenType::Operator && tok.text == "=") ||
                    (tok.type == TokenType::Keyword && tok.text == "return"))
                {
                    inValueContext = true;
                }
                else if (tok.type == TokenType::Semicolon || tok.type == TokenType::Comma)
                {
                    inValueContext = false;
                }
            }

            // Preprocessor
            if (tok.type == TokenType::Preprocessor)
            {
                flushCurrentLine();
                LineInfo prep;
                prep.isPreprocessor = true;
                // Indented with the block it sits in. Column zero is right for the `#include` at
                // the top of a file and only because that is already brace level zero; forcing it
                // everywhere tore a `#if` out of the function body it belongs to.
                prep.indentLevel = braceLevel;
                prep.tokenIndices.push_back(i);
                lines.push_back(std::move(prep));
                continue;
            }

            // Metadata block: `[Property, Category="Weapons"]` before a declaration.
            //
            // CScriptBuilder strips these before the compiler sees them, and the grammar makes the
            // block a sibling of the declaration rather than part of it, so it is its own line.
            // Told apart from an index expression by position alone: an index never opens a line -
            // `arr[0] = 1;` starts with the identifier - and metadata always does.
            if (tok.type == TokenType::OpenBracket && currentLine.tokenIndices.empty())
            {
                beginLineIfEmpty();
                currentLine.tokenIndices.push_back(i);
                int depth = 1;
                while (depth > 0 && i + 1 < tokens.size())
                {
                    ++i;
                    if (tokens[i].type == TokenType::OpenBracket)
                    {
                        depth++;
                        bracketDepth++;
                    }
                    else if (tokens[i].type == TokenType::CloseBracket)
                    {
                        depth--;
                        if (bracketDepth > 0) bracketDepth--;
                    }
                    currentLine.tokenIndices.push_back(i);
                }
                flushCurrentLine();
                continue;
            }

            // Open brace {
            if (tok.type == TokenType::OpenBrace)
            {
                // A value brace: already inside one, opened deeper than its enclosing scope (a
                // lambda or a list passed as an argument), or armed by an `=` or a `return`.
                const bool isValueBrace = currentScopeKind() == ScopeKind::Value ||
                                          parenDepth > baseParen || bracketDepth > baseBracket ||
                                          inValueContext;

                if (isValueBrace)
                {
                    // Statements or elements? A lambda body follows its parameter list, so a `)`
                    // or a bare `function` in front of the brace is the tell. Everything else -
                    // `= {`, `, {`, `( {`, `return {`, a list nested in a list - holds elements.
                    const bool padded = i > 0 && (tokens[i - 1].type == TokenType::CloseParen ||
                                                  tokens[i - 1].text == "function");
                    tokens[i].isPaddedBrace = padded;

                    beginLineIfEmpty();
                    currentLine.tokenIndices.push_back(i);
                    scopeStack.push_back({ ScopeKind::Value, parenDepth, bracketDepth, padded });
                    continue;
                }

                if (braceStyle == BraceStyle::KAndR && !currentLine.tokenIndices.empty())
                {
                    currentLine.tokenIndices.push_back(i);
                    flushCurrentLine();
                }
                else
                {
                    flushCurrentLine();
                    LineInfo braceLine;
                    braceLine.indentLevel = braceLevel;
                    braceLine.tokenIndices.push_back(i);
                    lines.push_back(std::move(braceLine));
                }

                scopeStack.push_back({ pendingScope, parenDepth, bracketDepth });
                pendingScope = ScopeKind::Generic;
                insideCaseBody = false;
                inValueContext = false;

                braceLevel++;
                continue;
            }

            // Close brace }
            if (tok.type == TokenType::CloseBrace)
            {
                // Closing a value: stays on the line its list or lambda body is on, and costs no
                // indent level, because opening it never took one.
                if (currentScopeKind() == ScopeKind::Value)
                {
                    tokens[i].isPaddedBrace = scopeStack.back().paddedBrace;
                    scopeStack.pop_back();
                    beginLineIfEmpty();
                    currentLine.tokenIndices.push_back(i);
                    continue;
                }

                flushCurrentLine();
                braceLevel = std::max(0, braceLevel - 1);
                if (!scopeStack.empty())
                {
                    scopeStack.pop_back();
                }
                insideCaseBody = false;
                inValueContext = false;

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

                // K&R puts it beside the brace that closed the `if`: `} else`. Only when that is
                // literally the line before - an `else` after a braceless `if` body has an
                // ordinary statement there and must not be glued onto it.
                if (braceStyle == BraceStyle::KAndR && !lines.empty() &&
                    lines.back().tokenIndices.size() == 1 &&
                    tokens[lines.back().tokenIndices.front()].type == TokenType::CloseBrace)
                {
                    // Taken back off the emitted list rather than appended to it, so the `{` that
                    // follows finds a line still open and lands on it too: `} else {`.
                    currentLine = std::move(lines.back());
                    lines.pop_back();
                    currentLine.tokenIndices.push_back(i);
                    continue;
                }

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
            beginLineIfEmpty();

            currentLine.tokenIndices.push_back(i);

            // A literal with no closing quote ends its line, whatever follows. Joining the next
            // line onto it would move that code inside the literal - the file would still be one
            // the compiler rejects, but for a different reason and in a different place, and the
            // user's own line breaks would be gone.
            if (tok.isUnterminated)
            {
                flushCurrentLine();
                continue;
            }

            // Semicolon outside paren/bracket ends statement. Inside a value scope it does not:
            // a lambda body written as an argument keeps its statements on the argument's line.
            if (tok.type == TokenType::Semicolon && parenDepth == 0 && bracketDepth == 0 &&
                currentScopeKind() != ScopeKind::Value)
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
                currentScopeKind() == ScopeKind::Enum)
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
                if (currentScopeKind() == ScopeKind::Switch)
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
        std::string result(bom);
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
        edit.range.start = lsp::Position{ 0, 0 };
        // Narrowing: size_t is 64-bit on x64, so the braced initialiser is a hard error there
        // even though the 32-bit build accepts it silently.
        edit.range.end = lsp::Position{ lineCount, static_cast<uint32_t>(lastLineLen) };
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
            FormattingRequest fullReq{ request.uri, request.sourceCode, request.tree, request.options, request.braceStyle };
            return FormatDocument(fullReq);
        }

        // Format document
        std::string fullFormatted = FormatSourceCode(request.sourceCode, request.options, request.braceStyle);
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

    std::optional<std::vector<lsp::TextEdit>> FormatOnType(const OnTypeFormattingRequest &request)
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
            request.uri,
            request.sourceCode,
            request.tree,
            lsp::Range{
                lsp::Position{ startLine, 0 },
                lsp::Position{ targetLine, request.position.character }
            },
            request.options,
            request.braceStyle
        };

        return FormatRange(rangeReq);
    }
}