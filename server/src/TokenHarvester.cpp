/**
 * @file TokenHarvester.cpp
 * @brief Implements fault-tolerant EBNF-guided grammar scanning for AngelScript LSP.
 */

#include "TokenHarvester.h"
#include "SafeCtype.h"
#include <sstream>
#include <algorithm>
#include <vector>
#include <map>
#include <fmt/core.h>
#include <fstream>
#include <iostream>

/**
 * @brief Prints internal debug logs to the standard error stream.
 * @param msg The log message string to output.
 */
static void HARVEST_DEBUG(const std::string &msg)
{
    std::cerr << "[Harvester] " << msg << std::endl;
}

namespace TokenHarvester
{
    /**
     * @brief Identifies if a token represents a scope or type declaration block.
     * @param token The keyword string to evaluate.
     * @return True if it is a class, interface, namespace, enum, or mixin, false otherwise.
     */
    static inline bool IsTypeDeclaration(std::string_view token)
    {
        return token == "class" || token == "interface" || token == "namespace" || token == "enum" || token == "mixin";
    }

    /**
     * @brief Identifies if a token represents an access modifier.
     * @param token The keyword string to evaluate.
     * @return True if it is a private, protected, public, or shared modifier, false otherwise.
     */
    static inline bool IsAccessModifier(std::string_view token)
    {
        return token == "private" || token == "protected" || token == "public" || token == "shared";
    }

    /**
     * @brief Identifies if a token represents a global modifier.
     * @param token The keyword string to evaluate.
     * @return True if it is an access modifier, external, or mixin, false otherwise.
     */
    static inline bool IsGlobalModifier(std::string_view token)
    {
        return IsAccessModifier(token) || token == "external" || token == "mixin";
    }

    /**
     * @brief Identifies if a token represents a property separator.
     * @param token The symbol string to evaluate.
     * @return True if it is a semicolon, equals sign, or comma, false otherwise.
     */
    static inline bool IsPropertySeparator(std::string_view token)
    {
        return token == ";" || token == "=" || token == ",";
    }

    /**
     * @brief Identifies if a token represents a built-in data type.
     * @param token The keyword string to evaluate.
     * @return True if it is a recognized built-in type, false otherwise.
     */
    static inline bool IsBuiltInType(std::string_view token)
    {
        return token == "void" || token == "int" || token == "int8" || token == "int16" || token == "int32" || token == "int64" ||
               token == "uint" || token == "uint8" || token == "uint16" || token == "uint32" || token == "uint64" ||
               token == "float" || token == "double" || token == "bool" || token == "auto" || token == "string" ||
               token == "array" || token == "dictionary" || token == "grid";
    }

    /**
     * @brief Identifies if a token represents a method modifier.
     * @param token The keyword string to evaluate.
     * @return True if it is a recognized method modifier, false otherwise.
     */
    static inline bool IsMethodModifier(std::string_view token)
    {
        return token == "const" || token == "override" || token == "final" || token == "property";
    }

    /**
     * @brief Identifies if a token represents a control keyword.
     * @param token The keyword string to evaluate.
     * @return True if it is a recognized control keyword, false otherwise.
     */
    static inline bool IsControlKeyword(std::string_view token)
    {
        return token == "if" || token == "for" || token == "foreach" || token == "while" ||
               token == "do" || token == "switch" || token == "try" || token == "catch" || token == "case";
    }

    /**
     * @class TokenStream
     * @brief Wrapper for the native AngelScript lexer to advance and peek tokens safely.
     */
    class TokenStream
    {
    private:
        asIScriptEngine *engine;
        std::string_view code;
        size_t pos;

    public:
        /**
         * @brief Constructs a new Token Stream object.
         * @param e Pointer to the active AngelScript script engine.
         * @param c The source code text view to parse.
         * @param start The initial absolute buffer position offset.
         */
        TokenStream(asIScriptEngine *e, std::string_view c, size_t start = 0)
            : engine(e), code(c), pos(start) {}

        /**
         * @brief Peeks at the upcoming token class without advancing the stream position.
         * @param outToken Reference to a string view to store the peeked token content.
         * @return The token class classification type.
         */
        asETokenClass Peek(std::string_view &outToken)
        {
            size_t tempPos = pos;
            asUINT len = 0;
            asETokenClass tc;

            do
            {
                if (tempPos >= code.length())
                {
                    outToken = "";
                    return asTC_UNKNOWN;
                }

                tc = engine->ParseToken(code.data() + tempPos, static_cast<asUINT>(code.length() - tempPos), &len);

                if (len == 0)
                {
                    outToken = "";
                    return asTC_UNKNOWN;
                }

                outToken = code.substr(tempPos, len);
                tempPos += len;

                if (tc == asTC_UNKNOWN && !outToken.empty() && outToken[0] == '#')
                {
                    while (tempPos < code.length() && code[tempPos] != '\n' && code[tempPos] != '\r')
                    {
                        tempPos++;
                    }
                    tc = asTC_WHITESPACE;
                }

            } while (tc == asTC_WHITESPACE || tc == asTC_COMMENT);

            return tc;
        }

        /**
         * @brief Advances the stream position to the next valid token.
         * @param outToken Reference to a string view to store the advanced token content.
         * @return The token class classification type of the extracted token.
         */
        asETokenClass Advance(std::string_view &outToken)
        {
            asUINT len = 0;
            asETokenClass tc;

            do
            {
                if (pos >= code.length())
                {
                    outToken = "";
                    return asTC_UNKNOWN;
                }

                tc = engine->ParseToken(code.data() + pos, static_cast<asUINT>(code.length() - pos), &len);

                if (len == 0)
                {
                    outToken = "";
                    return asTC_UNKNOWN;
                }

                outToken = code.substr(pos, len);
                pos += len;

                if (tc == asTC_UNKNOWN && !outToken.empty() && outToken[0] == '#')
                {
                    while (pos < code.length() && code[pos] != '\n' && code[pos] != '\r')
                    {
                        pos++;
                    }
                    tc = asTC_WHITESPACE;
                }

            } while (tc == asTC_WHITESPACE || tc == asTC_COMMENT);

            return tc;
        }

        /**
         * @brief Checks if the stream has reached the true end of the file.
         * @return True if the stream is at EOF, false otherwise.
         */
        bool IsEOF()
        {
            std::string_view dummy;
            asETokenClass tc = Peek(dummy);
            return (tc == asTC_UNKNOWN && dummy.empty());
        }

        /**
         * @brief Gets the current internal buffer position.
         * @return The current absolute index position.
         */
        size_t GetPos() const { return pos; }

        /**
         * @brief Sets the internal buffer position.
         * @param p The new absolute position offset to apply.
         */
        void SetPos(size_t p) { pos = p; }
    };

    /**
     * @brief Resolves and extracts the primitive base type from modifiers or templates.
     * @param type The raw data type description string.
     * @return A clean string representation of the parsed base data type.
     */
    std::string GetBaseType(const std::string &type)
    {
        std::string clean = type;
        size_t start = 0;
        size_t templatePos = std::string::npos;

        while (start < clean.length() && (clean[start] == ' ' || clean[start] == '\t'))
        {
            start++;
        }

        if (clean.substr(start, 6) == "const ")
        {
            start += 6;
        }

        while (start < clean.length() && (clean[start] == ' ' || clean[start] == '\t'))
        {
            start++;
        }

        clean = clean.substr(start);
        templatePos = clean.find('<');

        if (templatePos != std::string::npos)
        {
            clean = clean.substr(0, templatePos);
        }

        while (!clean.empty())
        {
            if (clean.back() == ' ' || clean.back() == '\t' || clean.back() == '\r' || clean.back() == '\n')
            {
                clean.pop_back();
                continue;
            }

            if (clean.length() >= 5 && clean.substr(clean.length() - 5) == "inout")
            {
                clean = clean.substr(0, clean.length() - 5);
                continue;
            }

            if (clean.length() >= 3 && clean.substr(clean.length() - 3) == "out")
            {
                clean = clean.substr(0, clean.length() - 3);
                continue;
            }

            if (clean.length() >= 2 && clean.substr(clean.length() - 2) == "in")
            {
                if (clean.length() == 2 || clean[clean.length() - 3] == ' ' || clean[clean.length() - 3] == '&')
                {
                    clean = clean.substr(0, clean.length() - 2);
                    continue;
                }
            }

            if (clean.back() == '&' || clean.back() == '@')
            {
                clean.pop_back();
                continue;
            }

            if (clean.length() >= 5 && clean.substr(clean.length() - 5) == "const")
            {
                clean = clean.substr(0, clean.length() - 5);
                continue;
            }

            break;
        }

        if (clean.find("[]") != std::string::npos)
        {
            return "array";
        }

        return clean;
    }

    /**
     * @brief Extracts the underlying parameter type wrapped inside native structural collections.
     * @param type The raw wrapper collection data type string.
     * @return A string representation of the isolated inner data type.
     */
    std::string ExtractInnerType(const std::string &type)
    {
        size_t start = type.find('<');
        size_t end = type.rfind('>');
        size_t bracket = type.rfind("[]");

        if (start != std::string::npos && end != std::string::npos && end > start)
        {
            return type.substr(start + 1, end - start - 1);
        }

        if (bracket != std::string::npos)
        {
            return type.substr(0, bracket);
        }

        return type;
    }

    /**
     * @brief Yields an instantiated script type stripped of type qualifiers.
     * @param type The qualified complex type string.
     * @return A clean string representing the instantiated script type.
     */
    std::string GetInstantiatedType(const std::string &type)
    {
        std::string clean = type;
        size_t start = 0;

        while (start < clean.length() && clean[start] == ' ')
        {
            start++;
        }

        if (clean.substr(start, 6) == "const ")
        {
            start += 6;
        }

        while (start < clean.length() && clean[start] == ' ')
        {
            start++;
        }

        clean = clean.substr(start);

        while (!clean.empty())
        {
            if (clean.back() == '@' || clean.back() == '&' || clean.back() == ' ')
            {
                clean.pop_back();
            }
            else if (clean.length() >= 5 && clean.substr(clean.length() - 5) == "const")
            {
                clean = clean.substr(0, clean.length() - 5);
            }
            else
            {
                break;
            }
        }

        return clean;
    }

    /**
     * @brief Verifies if a token safely maps to registered or recognized language types.
     * @param engine Pointer to the active AngelScript script engine core.
     * @param stream Reference to the current token stream processor.
     * @param token The token string contents to evaluate.
     * @param localClasses List of user-declared custom script classes.
     * @return True if the token is a valid recognized data type, false otherwise.
     */
    static bool IsValidDataType(asIScriptEngine *engine, TokenStream &stream, std::string_view token, const std::vector<std::string> &localClasses)
    {
        std::string tokenStr(token);
        size_t savedPos = 0;
        std::string_view dummy;

        if (token == "const")
        {
            return true;
        }

        if (IsBuiltInType(token))
        {
            return true;
        }

        if (engine && engine->GetTypeInfoByName(tokenStr.c_str()) != nullptr)
        {
            return true;
        }

        if (std::find(localClasses.begin(), localClasses.end(), tokenStr) != localClasses.end())
        {
            return true;
        }

        savedPos = stream.GetPos();
        stream.Advance(dummy);

        if (stream.Peek(dummy) == asTC_KEYWORD && dummy == "::")
        {
            stream.SetPos(savedPos);
            return true;
        }

        stream.SetPos(savedPos);
        return false;
    }

    /**
     * @brief Parses full semantic data type signatures from the stream including templates and handles.
     * @param engine Pointer to the active AngelScript script engine instance.
     * @param stream Reference to the token parsing stream.
     * @param knownClassNames List of globally identified class titles.
     * @return A constructed string representation of the final resolved data type signature.
     */
    static std::string ParseDataType(asIScriptEngine *engine, TokenStream &stream, const std::vector<std::string> &knownClassNames)
    {
        std::string typeName = "";
        std::string_view token;
        asETokenClass tc = stream.Peek(token);
        bool hasValidRoot = false;
        int templateDepth = 0;

        if (tc == asTC_KEYWORD && token == "const")
        {
            typeName += "const ";
            stream.Advance(token);
            tc = stream.Peek(token);
        }

        if (tc == asTC_KEYWORD && token == "::")
        {
            typeName += "::";
            stream.Advance(token);
            tc = stream.Peek(token);
        }

        while (tc == asTC_IDENTIFIER || (tc == asTC_KEYWORD && IsBuiltInType(token)))
        {
            hasValidRoot = true;
            typeName += std::string(token);
            stream.Advance(token);
            tc = stream.Peek(token);

            if (tc == asTC_KEYWORD && token == "::")
            {
                typeName += "::";
                stream.Advance(token);
                tc = stream.Peek(token);
            }
            else
            {
                break;
            }
        }

        if (!hasValidRoot)
        {
            return "";
        }

        if (tc == asTC_KEYWORD && token == "<")
        {
            typeName += "<";
            stream.Advance(token);
            templateDepth = 1;
            static_cast<void>(templateDepth);

            while (templateDepth > 0 && !stream.IsEOF())
            {
                tc = stream.Peek(token);

                if (tc == asTC_KEYWORD && token == "<")
                {
                    templateDepth++;
                }
                else if (tc == asTC_KEYWORD && token == ">")
                {
                    templateDepth--;
                }

                typeName += std::string(token);
                stream.Advance(token);
            }

            tc = stream.Peek(token);
        }

        while (tc == asTC_KEYWORD && token == "@")
        {
            typeName += "@";
            stream.Advance(token);
            tc = stream.Peek(token);

            if (tc == asTC_KEYWORD && token == "const")
            {
                typeName += "const";
                stream.Advance(token);
                tc = stream.Peek(token);
            }
        }

        if (tc == asTC_KEYWORD && (token == "&" || token == "&in" || token == "&out" || token == "&inout"))
        {
            stream.Advance(token);
            typeName += std::string(token);

            if (token == "&")
            {
                tc = stream.Peek(token);

                if ((tc == asTC_KEYWORD || tc == asTC_IDENTIFIER) &&
                    (token == "in" || token == "out" || token == "inout"))
                {
                    stream.Advance(token);
                    typeName += " " + std::string(token);
                }
            }
        }

        return typeName;
    }

    /**
     * @brief Converts line and character indices to an absolute character position index.
     * @param text The source document layout view.
     * @param line Zero-indexed target script row coordinate.
     * @param character Zero-indexed target script column offset.
     * @return The precise flat buffer offset index size_t value.
     */
    size_t GetAbsolutePosition(std::string_view text, int line, int character)
    {
        size_t pos = 0;
        int currentLine = 0;

        while (currentLine < line && pos < text.length())
        {
            if (text[pos] == '\n')
            {
                currentLine++;
            }

            pos++;
        }

        return pos + character;
    }

    /**
     * @brief Contextually evaluates token chains preceding a cursor position to identify completion environments.
     * @param engine Pointer to the active script engine compiler backend.
     * @param code The script scope code layout view.
     * @param cursorAbsolutePos Absolute offset index indicating the location of the autocomplete trigger.
     * @return A detailed context specification collection object mapping properties.
     */
    CompletionContext GetCompletionContext(asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos)
    {
        CompletionContext ctx;
        TokenStream stream(engine, code);
        std::string_view token;
        std::vector<std::string> tokens;
        std::vector<std::string> chain;
        std::string lastTok = "";
        std::string fullIdent = "";
        std::string closingToken = "";
        std::string openingToken = "";
        std::string segment = "";
        std::string tok = "";
        int i = 0;
        int depth = 0;

        ctx.isMemberAccess = false;
        ctx.partialMember = "";
        ctx.lastSeparator = "";

        while (stream.GetPos() < cursorAbsolutePos && !stream.IsEOF())
        {
            stream.Advance(token);
            tokens.push_back(std::string(token));
        }

        if (tokens.empty())
        {
            return ctx;
        }

        i = (int)tokens.size() - 1;

        if (tokens[i] == "." || tokens[i] == "::")
        {
            ctx.isMemberAccess = true;
            ctx.lastSeparator = tokens[i];
            i--;
        }
        else if (i >= 1 && (tokens[i - 1] == "." || tokens[i - 1] == "::"))
        {
            ctx.isMemberAccess = true;
            ctx.partialMember = tokens[i];
            ctx.lastSeparator = tokens[i - 1];
            i -= 2;
        }
        else
        {
            lastTok = tokens[i];

            if (!lastTok.empty() && (SAFE_IS_ALPHA(lastTok[0]) || lastTok[0] == '_'))
            {
                ctx.partialMember = lastTok;
                i--;
            }

            if (i >= 0 && tokens[i] == ":")
            {
                ctx.lastSeparator = ":";

                if (i >= 1)
                {
                    ctx.objectChain.push_back(tokens[i - 1]);
                }
            }
        }

        if (ctx.isMemberAccess)
        {
            while (i >= 0)
            {
                fullIdent = "";

                while (i >= 0 && (tokens[i] == "]" || tokens[i] == ")"))
                {
                    closingToken = tokens[i];
                    openingToken = (closingToken == "]") ? "[" : "(";
                    depth = 0;
                    segment = "";

                    do
                    {
                        if (tokens[i] == closingToken)
                        {
                            depth++;
                        }
                        else if (tokens[i] == openingToken)
                        {
                            depth--;
                        }

                        segment = tokens[i] + segment;
                        i--;

                    } while (i >= 0 && depth > 0);

                    fullIdent = segment + fullIdent;
                }

                if (i >= 0 && tokens[i] != "." && tokens[i] != "::")
                {
                    tok = tokens[i];

                    if (!tok.empty() && (SAFE_IS_ALPHA(tok[0]) || tok[0] == '_' || tok == "this" || tok == "super"))
                    {
                        fullIdent = tok + fullIdent;
                        chain.insert(chain.begin(), fullIdent);
                        i--;
                    }
                    else
                    {
                        break;
                    }
                }
                else if (!fullIdent.empty())
                {
                    chain.insert(chain.begin(), fullIdent);
                }

                if (i >= 0 && (tokens[i] == "." || tokens[i] == "::"))
                {
                    i--;
                }
                else
                {
                    break;
                }
            }

            ctx.objectChain = chain;
        }

        return ctx;
    }

    /**
     * @class ASTScanner
     * @brief Base parser class analyzing block architecture layouts to maintain scope state configurations.
     */
    class ASTScanner
    {
    protected:
        /**
         * @struct ScopeCtx
         * @brief Representation tracking nested lexical scope structural environments.
         */
        struct ScopeCtx
        {
            std::string name;
            std::string type;
            int depth;
        };

        asIScriptEngine *engine;
        std::string_view code;
        TokenStream stream;
        std::vector<std::string> knownClassNames;
        std::vector<ScopeCtx> scopeStack;
        std::string_view token;
        std::string pendingScopeType;
        std::string pendingScopeName;
        int currentDepth;

    public:
        /**
         * @brief Constructs a base ASTScanner component.
         * @param e Pointer to the active host AngelScript script compiler layout module.
         * @param c Source character text sequence tracking context view.
         */
        ASTScanner(asIScriptEngine *e, std::string_view c)
            : engine(e), code(c), stream(e, c), currentDepth(0) {}

    protected:
        /**
         * @brief Unifies active scope strings to construct a scope resolution string identifier.
         * @return Fully structural qualified contextual resolution chain namespace string.
         */
        std::string GetFullScope() const
        {
            std::string res;

            for (const auto &s : scopeStack)
            {
                if (!res.empty())
                {
                    res += "::";
                }

                res += s.name;
            }

            return res;
        }

        /**
         * @brief Fast-forwards layout scanning sequences until nested braces resolve symmetrically.
         */
        void SkipBlock()
        {
            int methodDepth = 1;

            while (methodDepth > 0 && !stream.IsEOF())
            {
                stream.Advance(token);

                if (token == "{")
                {
                    methodDepth++;
                }
                else if (token == "}")
                {
                    methodDepth--;
                }
            }
        }

        /**
         * @brief Executes basic preliminary passes registering namespace and object tags.
         */
        void CollectKnownTypes()
        {
            TokenStream quickStream(engine, code);
            std::vector<std::string> quickScope;
            std::string qPendingName = "";
            std::string qPendingType = "";
            std::string fScope = "";
            asETokenClass tc = asTC_UNKNOWN;
            int qDepth = 0;

            while (!quickStream.IsEOF())
            {
                tc = quickStream.Advance(token);

                if (tc == asTC_KEYWORD && IsTypeDeclaration(token))
                {
                    qPendingType = std::string(token);

                    if (quickStream.Peek(token) == asTC_IDENTIFIER)
                    {
                        qPendingName = std::string(token);
                        quickStream.Advance(token);
                    }
                }
                else if (tc == asTC_KEYWORD && token == "{")
                {
                    qDepth++;

                    if (!qPendingName.empty())
                    {
                        quickScope.push_back(qPendingName);
                        fScope = "";

                        for (auto &s : quickScope)
                        {
                            fScope += (fScope.empty() ? "" : "::") + s;
                        }

                        knownClassNames.push_back(fScope);
                        qPendingName = "";
                    }
                }
                else if (tc == asTC_KEYWORD && token == "}")
                {
                    if (!quickScope.empty())
                    {
                        quickScope.pop_back();
                    }

                    qDepth--;
                }
            }
        }
    };

    /**
     * @class CustomClassScanner
     * @brief Specialized class tracker identifying structural object components, declarations, and method rows.
     */
    class CustomClassScanner : public ASTScanner
    {
    private:
        std::map<std::string, ScriptClass> classMap;
        std::string currentAccess;

    public:
        /**
         * @brief Constructs a new Custom Class Scanner object processor.
         * @param e Handle context pointing to the current script environment instance.
         * @param c Comprehensive code layout script source blueprint view string.
         */
        CustomClassScanner(asIScriptEngine *e, std::string_view c) : ASTScanner(e, c), currentAccess("public")
        {
            CollectKnownTypes();
        }

        /**
         * @brief Scans through files isolating all declared object layout descriptions.
         * @return Discovered records mapping definitions extracted from structures.
         */
        std::vector<ScriptClass> Scan()
        {
            std::vector<std::string> bases;
            std::string fullScope = "";
            std::vector<ScriptClass> results;
            size_t savedPos = 0;
            asETokenClass tc = asTC_UNKNOWN;
            asETokenClass peekTc = asTC_UNKNOWN;

            while (!stream.IsEOF())
            {
                savedPos = stream.GetPos();
                tc = stream.Advance(token);

                if (tc == asTC_KEYWORD && IsTypeDeclaration(token))
                {
                    pendingScopeType = std::string(token);

                    if (stream.Peek(token) == asTC_IDENTIFIER)
                    {
                        pendingScopeName = std::string(token);
                        stream.Advance(token);
                        bases.clear();

                        if (stream.Peek(token) == asTC_KEYWORD && token == ":")
                        {
                            stream.Advance(token);

                            while (!stream.IsEOF())
                            {
                                peekTc = stream.Peek(token);
                                if (peekTc == asTC_KEYWORD && token == "{")
                                {
                                    break;
                                }

                                stream.Advance(token);

                                if (token != "," && token != "public" && token != "protected" && token != "private")
                                {
                                    bases.push_back(std::string(token));
                                }
                            }
                        }

                        if (stream.Peek(token) == asTC_KEYWORD && token == "{")
                        {
                            currentDepth++;
                            scopeStack.push_back({pendingScopeName, pendingScopeType, currentDepth});
                            fullScope = GetFullScope();

                            if (classMap.find(fullScope) == classMap.end())
                            {
                                classMap[fullScope] = ScriptClass{fullScope, bases, {}, {}};
                            }

                            pendingScopeName = "";
                            pendingScopeType = "";
                            currentAccess = "public";
                            stream.Advance(token);
                        }
                    }

                    continue;
                }

                if (tc == asTC_KEYWORD && token == "{")
                {
                    currentDepth++;

                    if (!pendingScopeName.empty())
                    {
                        scopeStack.push_back({pendingScopeName, pendingScopeType, currentDepth});
                        fullScope = GetFullScope();

                        if (classMap.find(fullScope) == classMap.end())
                        {
                            classMap[fullScope] = ScriptClass{fullScope, {}, {}, {}};
                        }

                        pendingScopeName = "";
                        pendingScopeType = "";
                        currentAccess = "public";
                    }

                    continue;
                }

                if (tc == asTC_KEYWORD && token == "}")
                {
                    if (!scopeStack.empty() && scopeStack.back().depth == currentDepth)
                    {
                        scopeStack.pop_back();
                        currentAccess = "public";
                    }

                    currentDepth--;
                    continue;
                }

                if (tc == asTC_KEYWORD && IsAccessModifier(token))
                {
                    if (token != "shared")
                    {
                        currentAccess = std::string(token);
                    }

                    continue;
                }

                if (scopeStack.empty())
                {
                    continue;
                }

                if (ProcessEnum(tc))
                {
                    continue;
                }

                bool validDeclFound = ProcessConstructorOrDestructor(tc);

                if (!validDeclFound)
                {
                    validDeclFound = ProcessMember(savedPos);
                }

                if (validDeclFound)
                {
                    currentAccess = "public";
                }
                else if (stream.GetPos() == savedPos)
                {
                    stream.Advance(token);
                }
            }

            for (auto &pair : classMap)
            {
                results.push_back(pair.second);
            }

            return results;
        }

    private:
        /**
         * @brief Extracts individual element identifiers listed within isolated enumerations.
         * @param tc Core structural classification code.
         * @return True if an enumeration member matches and updates successfully, false otherwise.
         */
        bool ProcessEnum(asETokenClass tc)
        {
            if (scopeStack.back().type != "enum")
            {
                return false;
            }

            if (tc == asTC_IDENTIFIER)
            {
                classMap[GetFullScope()].properties.push_back({std::string(token), "int", "public"});
            }

            return true;
        }

        /**
         * @brief Validates method symbols checking formatting configurations targeting lifecycle blocks.
         * @param tc Native token context structural classification.
         * @return True if matching constructor/destructor operations parse correctly, false otherwise.
         */
        bool ProcessConstructorOrDestructor(asETokenClass tc)
        {
            ScopeCtx currentScope;
            std::string fullScope = "";

            if (scopeStack.empty())
            {
                return false;
            }

            currentScope = scopeStack.back();
            fullScope = GetFullScope();

            if (tc == asTC_IDENTIFIER && token == currentScope.name)
            {
                if (stream.Peek(token) == asTC_KEYWORD && token == "(")
                {
                    classMap[fullScope].methods.push_back({currentScope.name, "void", fullScope + "()", currentAccess, true});
                    SkipToBody();
                    return true;
                }
            }
            else if (tc == asTC_KEYWORD && token == "~")
            {
                if (stream.Peek(token) == asTC_IDENTIFIER && token == currentScope.name)
                {
                    stream.Advance(token);

                    if (stream.Peek(token) == asTC_KEYWORD && token == "(")
                    {
                        classMap[fullScope].methods.push_back({"~" + currentScope.name, "void", "~" + fullScope + "()", currentAccess, true});
                        SkipToBody();
                        return true;
                    }
                }
            }

            return false;
        }

        /**
         * @brief Skips token tracking layers up to structural code body frames or delimiter endings.
         */
        void SkipToBody()
        {
            while (!stream.IsEOF() && token != "{" && token != ";")
            {
                stream.Advance(token);
            }

            if (stream.Peek(token) == asTC_KEYWORD && token == "{")
            {
                stream.Advance(token);
                SkipBlock();
            }
            else if (stream.Peek(token) == asTC_KEYWORD && token == ";")
            {
                stream.Advance(token);
            }
        }

        /**
         * @brief Parses details configuring individual internal class rows or operations signatures.
         * @param savedPos Entry positional fallback index utilized to reset states during evaluation conflicts.
         * @return True if a class attribute or method row matches criteria, false otherwise.
         */
        bool ProcessMember(size_t savedPos)
        {
            std::string parsedType = "";
            std::string memberName = "";
            std::string fullScope = "";
            asETokenClass lookTc = asTC_UNKNOWN;

            stream.SetPos(savedPos);
            parsedType = ParseDataType(engine, stream, knownClassNames);

            if (parsedType.empty() || stream.Peek(token) != asTC_IDENTIFIER)
            {
                stream.SetPos(savedPos);
                return false;
            }

            memberName = std::string(token);
            stream.Advance(token);
            lookTc = stream.Peek(token);

            if (lookTc == asTC_KEYWORD && token == "(")
            {
                ParseMethod(parsedType, memberName);
                return true;
            }

            if (lookTc == asTC_KEYWORD && IsPropertySeparator(token))
            {
                ParseProperty(parsedType, memberName);
                return true;
            }

            if (lookTc == asTC_KEYWORD && token == "{")
            {
                fullScope = GetFullScope();
                classMap[fullScope].properties.push_back({memberName, parsedType, currentAccess});
                stream.Advance(token);
                SkipBlock();
                return true;
            }

            stream.SetPos(savedPos);
            return false;
        }

        /**
         * @brief Extracts descriptive parameter layouts and logic details structuring class functions.
         * @param parsedType Datatype designation describing operation output returns.
         * @param memberName Plain identifier naming the current function method target.
         */
        void ParseMethod(const std::string &parsedType, const std::string &memberName)
        {
            std::string fullScope = "";
            std::string exactParams = "";
            std::string signature = "";
            size_t sigStart = 0;
            size_t sigEnd = 0;
            int parenDepth = 0;

            fullScope = GetFullScope();
            sigStart = stream.GetPos();

            do
            {
                stream.Advance(token);

                if (token == "(")
                {
                    parenDepth++;
                }
                else if (token == ")")
                {
                    parenDepth--;
                }

            } while (parenDepth > 0 && !stream.IsEOF());

            sigEnd = stream.GetPos();
            exactParams = std::string(code.data() + sigStart, sigEnd - sigStart);
            signature = parsedType + " " + fullScope + "::" + memberName + exactParams;

            classMap[fullScope].methods.push_back({memberName, parsedType, signature, currentAccess, false});

            while (stream.Peek(token) == asTC_IDENTIFIER || (stream.Peek(token) == asTC_KEYWORD && IsMethodModifier(token)))
            {
                stream.Advance(token);
            }

            if (stream.Peek(token) == asTC_KEYWORD && token == "{")
            {
                stream.Advance(token);
                SkipBlock();
            }
            else if (stream.Peek(token) == asTC_KEYWORD && token == ";")
            {
                stream.Advance(token);
            }
        }

        /**
         * @brief Isolates properties resolving syntax setups processing shared definition tracks.
         * @param parsedType Clean data description tracking label specifications.
         * @param memberName Plain descriptor detailing primary class properties labels.
         */
        void ParseProperty(const std::string &parsedType, const std::string &memberName)
        {
            std::string fullScope = "";
            std::string nextVar = "";
            asETokenClass tcc = asTC_UNKNOWN;

            fullScope = GetFullScope();
            classMap[fullScope].properties.push_back({memberName, parsedType, currentAccess});

            while (true)
            {
                tcc = stream.Peek(token);

                if (tcc == asTC_UNKNOWN || (tcc == asTC_KEYWORD && (token == ";" || token == ")")))
                {
                    stream.Advance(token);
                    break;
                }

                if (tcc == asTC_KEYWORD && token == ",")
                {
                    stream.Advance(token);

                    if (stream.Peek(token) == asTC_IDENTIFIER)
                    {
                        nextVar = std::string(token);
                        stream.Advance(token);
                        classMap[fullScope].properties.push_back({nextVar, parsedType, currentAccess});
                        continue;
                    }
                }
                else
                {
                    stream.Advance(token);
                }
            }
        }
    };

    /**
     * @brief High-level helper starting parsing routines targeting file script structures.
     * @param engine Handle referencing the host scripting system controller framework module.
     * @param code View covering active text sequences data streams under review.
     * @return Tracking list mapping individual user definitions fields.
     */
    std::vector<ScriptClass> ScanCustomClasses(asIScriptEngine *engine, std::string_view code)
    {
        CustomClassScanner scanner(engine, code);
        return scanner.Scan();
    }

    /**
     * @class LocalVariableScanner
     * @brief Specialized tracking parser tracing execution segments to extract localized scope variables.
     */
    class LocalVariableScanner : public ASTScanner
    {
    private:
        /**
         * @struct BlockCtxInfo
         * @brief Representation recording metrics mapping structural tracking data.
         */
        struct BlockCtxInfo
        {
            std::string type;
            int depth;
        };

        std::vector<LocalVariable> locals;
        std::vector<BlockCtxInfo> blockStack;
        std::string lastControlKeyword;
        std::string lastTokenStr;
        size_t cursorAbsolutePos;
        int parenDepth;

        const std::vector<ScriptClass> &customClasses;
        const std::vector<GlobalVariable> &globalVars;
        const std::vector<GlobalFunction> &globalFuncs;

    public:
        /**
         * @brief Constructs a new Local Variable Scanner system context handle.
         * @param e Main target script platform application management pointer.
         * @param c Continuous text mapping raw project layouts description structures views.
         * @param pos Cursor location indicator setting limits tracking absolute ranges.
         * @param cClasses Cached definitions catalog detailing active structure environments profiles.
         * @param gVars Configuration logs tracking file level variable environments registers.
         * @param gFuncs List capturing functional descriptions mapping global routines components.
         */
        LocalVariableScanner(asIScriptEngine *e, std::string_view c, size_t pos,
                             const std::vector<ScriptClass> &cClasses,
                             const std::vector<GlobalVariable> &gVars,
                             const std::vector<GlobalFunction> &gFuncs)
            : ASTScanner(e, c), lastControlKeyword(""), parenDepth(0), cursorAbsolutePos(pos),
              customClasses(cClasses), globalVars(gVars), globalFuncs(gFuncs)
        {
            CollectKnownTypes();
        }

        /**
         * @brief Maps structural context metrics into explicit human-readable definitions strings.
         * @param pDepth Tracking tracker monitoring nesting depths across parenthesized groupings tokens.
         * @param effDepth Quantitative reference setting target absolute layer context levels.
         * @return Informative literal highlighting the identified variable usage context category.
         */
        std::string GetVariableNature(int pDepth, int effDepth) const
        {
            if (pDepth > 0)
            {
                if (lastControlKeyword == "for")
                {
                    return "for loop control variable";
                }

                if (blockStack.empty())
                {
                    return "function parameter";
                }
            }

            for (auto it = blockStack.rbegin(); it != blockStack.rend(); ++it)
            {
                if (it->type == "for" || it->type == "while" || it->type == "foreach" || it->type == "do")
                {
                    return fmt::format("local variable inside a {} loop block", it->type);
                }

                if (it->type == "if")
                {
                    return "local variable inside an if block";
                }

                if (it->type == "switch" || it->type == "case")
                {
                    return "local variable inside a switch case";
                }

                if (it->type == "try" || it->type == "catch")
                {
                    return "local variable inside a try/catch block";
                }

                if (it->type == "function")
                {
                    return "local variable";
                }
            }

            return "local variable";
        }

        /**
         * @brief Parses through source layout configurations up to target positioning thresholds.
         * @param outEnclosingClass Receives the text label naming the parent class scope wrapper context.
         * @return List outlining stack values identified as fully accessible.
         */
        std::vector<LocalVariable> Scan(std::string &outEnclosingClass)
        {
            std::string foreachType = "";
            std::string loopVar = "";
            std::vector<std::string> containerExpr;
            std::string containerType = "";
            std::string inner = "";
            std::string blockType = "";
            size_t savedPos = 0;
            size_t foreachPos = 0;
            asETokenClass tc = asTC_UNKNOWN;
            asETokenClass peekTc = asTC_UNKNOWN;
            int captureDepth = 0;
            bool directlyInClass = false;
            bool validDeclFound = false;
            bool insideClassDecl = false;

            while (stream.GetPos() < cursorAbsolutePos && !stream.IsEOF())
            {
                savedPos = stream.GetPos();
                tc = stream.Peek(token);

                if (tc == asTC_KEYWORD && token == "function")
                {
                    stream.Advance(token);
                    if (stream.Peek(token) == asTC_KEYWORD && token == "(")
                    {
                        stream.Advance(token);
                        while (!stream.IsEOF() && token != ")")
                        {
                            std::string paramType = ParseDataType(engine, stream, knownClassNames);
                            if (!paramType.empty() && stream.Peek(token) == asTC_IDENTIFIER)
                            {
                                std::string paramName = std::string(token);
                                stream.Advance(token);
                                locals.push_back({paramName, paramType, currentDepth + 1});
                                std::string scopePrefix = GetFullScope().empty() ? "::" : GetFullScope() + "::";
                                HARVEST_DEBUG(fmt::format("NEW LOCAL VARIABLE: '{}{}' of type '{}' (lambda parameter) at depth {}",
                                                          scopePrefix, paramName, paramType, currentDepth + 1));
                            }
                            else
                            {
                                stream.Advance(token);
                            }
                            if (stream.Peek(token) == asTC_KEYWORD && token == ",")
                            {
                                stream.Advance(token);
                            }
                        }
                        if (stream.Peek(token) == asTC_KEYWORD && token == ")")
                        {
                            stream.Advance(token);
                        }
                    }
                    lastTokenStr = ")";
                    continue;
                }

                if (tc == asTC_KEYWORD)
                {
                    if (IsControlKeyword(token))
                    {
                        lastControlKeyword = std::string(token);
                    }
                    else if (token == ";")
                    {
                        if (parenDepth == 0)
                        {
                            lastControlKeyword = "";
                        }
                    }
                }

                if (tc == asTC_KEYWORD && IsTypeDeclaration(token))
                {
                    pendingScopeType = std::string(token);
                    stream.Advance(token);
                    lastTokenStr = pendingScopeType;

                    if (stream.Peek(token) == asTC_IDENTIFIER)
                    {
                        pendingScopeName = std::string(token);
                        stream.Advance(token);
                        lastTokenStr = pendingScopeName;
                    }

                    continue;
                }

                if (tc == asTC_KEYWORD && token == "{")
                {
                    currentDepth++;
                    blockType = "generic";

                    if (!lastControlKeyword.empty())
                    {
                        blockType = lastControlKeyword;
                        lastControlKeyword = "";
                    }
                    else
                    {
                        insideClassDecl = (!scopeStack.empty() && scopeStack.back().type == "class" && (currentDepth - 1) == scopeStack.back().depth);

                        if (scopeStack.empty() || insideClassDecl)
                        {
                            blockType = "function";
                        }
                    }

                    blockStack.push_back({blockType, currentDepth});

                    if (!pendingScopeName.empty())
                    {
                        scopeStack.push_back({pendingScopeName, pendingScopeType, currentDepth});
                        pendingScopeName = "";
                        pendingScopeType = "";
                    }

                    stream.Advance(token);
                    lastTokenStr = "{";
                    continue;
                }

                if (tc == asTC_KEYWORD && token == "}")
                {
                    if (!blockStack.empty() && blockStack.back().depth == currentDepth)
                    {
                        blockStack.pop_back();
                    }

                    if (!scopeStack.empty() && scopeStack.back().depth == currentDepth)
                    {
                        scopeStack.pop_back();
                    }

                    currentDepth--;
                    captureDepth = currentDepth;

                    locals.erase(std::remove_if(locals.begin(), locals.end(),
                                                [captureDepth](const LocalVariable &v)
                                                { return v.declarationDepth > captureDepth; }),
                                 locals.end());

                    stream.Advance(token);
                    lastTokenStr = "}";
                    continue;
                }

                if (tc == asTC_KEYWORD && token == "foreach")
                {
                    stream.Advance(token);

                    if (stream.Peek(token) == asTC_KEYWORD && token == "(")
                    {
                        stream.Advance(token);
                        parenDepth++;
                        foreachPos = stream.GetPos();
                        foreachType = ParseDataType(engine, stream, knownClassNames);

                        if (!foreachType.empty() && stream.Peek(token) == asTC_IDENTIFIER)
                        {
                            loopVar = std::string(token);
                            stream.Advance(token);
                            containerExpr.clear();

                            while (!stream.IsEOF() && token != ")")
                            {
                                stream.Advance(token);

                                if (token == ":")
                                {
                                    while (!stream.IsEOF() && token != ")")
                                    {
                                        stream.Advance(token);
                                        containerExpr.push_back(std::string(token));
                                    }

                                    break;
                                }
                            }

                            if (foreachType.find("auto") != std::string::npos && !containerExpr.empty())
                            {
                                containerType = InferAutoType(containerExpr);
                                inner = ExtractInnerType(containerType);

                                if (!inner.empty() && inner != containerType)
                                {
                                    if (foreachType.back() == '@' && inner.back() != '@')
                                    {
                                        foreachType = inner + "@";
                                    }
                                    else
                                    {
                                        foreachType = inner;
                                    }
                                }
                            }

                            locals.push_back({loopVar, foreachType, currentDepth + 1});
                            std::string scopePrefix = GetFullScope().empty() ? "::" : GetFullScope() + "::";

                            HARVEST_DEBUG(fmt::format("NEW LOCAL VARIABLE: '{}{}' of type '{}' (foreach loop variable) at depth {}",
                                                      scopePrefix, loopVar, foreachType, currentDepth + 1));
                        }
                        else
                        {
                            stream.SetPos(foreachPos);
                        }
                    }

                    continue;
                }

                while (tc == asTC_KEYWORD && IsAccessModifier(token))
                {
                    stream.Advance(token);
                    lastTokenStr = std::string(token);
                    tc = stream.Peek(token);
                }

                directlyInClass = false;

                if (!scopeStack.empty() && scopeStack.back().type == "class" && currentDepth == scopeStack.back().depth)
                {
                    directlyInClass = true;
                }

                validDeclFound = false;

                if (lastTokenStr != "." && lastTokenStr != "->" && lastTokenStr != "::")
                {
                    validDeclFound = ProcessLocalDeclaration(savedPos, directlyInClass);
                }

                if (!validDeclFound && stream.GetPos() == savedPos)
                {
                    stream.Advance(token);

                    if (token == "(")
                    {
                        parenDepth++;
                    }
                    else if (token == ")")
                    {
                        if (parenDepth > 0)
                        {
                            parenDepth--;
                        }
                    }

                    lastTokenStr = std::string(token);
                }
            }

            if (!scopeStack.empty() && scopeStack.back().type == "class")
            {
                outEnclosingClass = GetFullScope();
                locals.push_back({"this", outEnclosingClass + "@", 1});
            }

            return locals;
        }

    private:
        /**
         * @brief Evaluates code syntax configurations resolving exact datatypes matching abstract auto labels.
         * @param tokens Sequence tracking components formatting the active declaration row layout.
         * @return The inferred specific script type name signature.
         */
        std::string InferAutoType(const std::vector<std::string> &tokens)
        {
            std::string inferredType = "";
            std::string currentEnclosingClass = "";
            std::string tok = "";
            std::string base = "";
            std::string nextType = "";
            std::string baseType = "";
            asITypeInfo *t = nullptr;
            asIScriptFunction *func = nullptr;
            const char *decl = nullptr;
            const char *pName = nullptr;
            size_t i = 0;
            int tempBracketDepth = 0;
            int tempParenDepth = 0;
            int pTypeId = 0;
            bool found = false;

            if (tokens.empty())
            {
                return "auto";
            }

            if (IsBuiltInType(tokens[0]))
            {
                return std::string(tokens[0]);
            }

            if (tokens[0][0] == '"' || tokens[0][0] == '\'')
            {
                return "string";
            }

            if (tokens[0] == "true" || tokens[0] == "false")
            {
                return "bool";
            }

            if (tokens[0].find_first_of(".f") != std::string::npos && isdigit(static_cast<unsigned char>(tokens[0][0])))
            {
                return "float";
            }

            if (isdigit(static_cast<unsigned char>(tokens[0][0])))
            {
                return "int";
            }

            currentEnclosingClass = GetFullScope();

            for (i = 0; i < tokens.size(); i++)
            {
                tok = tokens[i];

                if (tok == "[")
                {
                    if (tempBracketDepth == 0)
                    {
                        base = GetBaseType(inferredType);

                        if (base == "array" || base == "dictionary" || base == "grid")
                        {
                            inferredType = ExtractInnerType(inferredType);
                        }
                        else
                        {
                            nextType = "";
                            t = engine ? engine->GetTypeInfoByDecl(inferredType.c_str()) : nullptr;

                            if (!t && engine)
                            {
                                t = engine->GetTypeInfoByName(base.c_str());
                            }

                            if (t)
                            {
                                for (asUINT m = 0; m < t->GetMethodCount(); m++)
                                {
                                    func = t->GetMethodByIndex(m);

                                    if (func && std::string(func->GetName()) == "opIndex")
                                    {
                                        decl = engine->GetTypeDeclaration(func->GetReturnTypeId(), true);

                                        if (decl)
                                        {
                                            nextType = decl;
                                        }

                                        break;
                                    }
                                }
                            }

                            if (nextType.empty())
                            {
                                for (const auto &c : customClasses)
                                {
                                    if (c.name == base)
                                    {
                                        for (const auto &m : c.methods)
                                        {
                                            if (m.name == "opIndex")
                                            {
                                                nextType = m.typeName;
                                                break;
                                            }
                                        }

                                        break;
                                    }
                                }
                            }

                            if (!nextType.empty())
                            {
                                inferredType = GetInstantiatedType(nextType);
                            }
                        }
                    }

                    tempBracketDepth++;
                    continue;
                }

                if (tok == "]")
                {
                    tempBracketDepth--;
                    continue;
                }

                if (tok == "(")
                {
                    tempParenDepth++;
                    continue;
                }

                if (tok == ")")
                {
                    tempParenDepth--;
                    continue;
                }

                if (tempBracketDepth > 0 || tempParenDepth > 0)
                {
                    continue;
                }

                if (tok == "." || tok == "::" || tok == "new" || tok == "@")
                {
                    continue;
                }

                if (!isalpha(static_cast<unsigned char>(tok[0])) && tok[0] != '_')
                {
                    continue;
                }

                if (inferredType.empty())
                {
                    if (tok == "this")
                    {
                        inferredType = currentEnclosingClass;
                    }
                    else
                    {
                        for (const auto &v : locals)
                        {
                            if (v.name == tok)
                            {
                                inferredType = v.typeName;
                                break;
                            }
                        }
                    }
                }
                else
                {
                    baseType = GetBaseType(inferredType);
                    found = false;

                    for (const auto &c : customClasses)
                    {
                        if (c.name == baseType)
                        {
                            for (const auto &p : c.properties)
                            {
                                if (p.name == tok)
                                {
                                    inferredType = p.typeName;
                                    found = true;
                                    break;
                                }
                            }

                            if (!found)
                            {
                                for (const auto &m : c.methods)
                                {
                                    if (m.name == "tok")
                                    {
                                        inferredType = m.typeName;
                                        found = true;
                                        break;
                                    }
                                }
                            }

                            break;
                        }
                    }

                    if (!found && engine)
                    {
                        t = engine->GetTypeInfoByName(baseType.c_str());

                        if (t)
                        {
                            for (asUINT p = 0; p < t->GetPropertyCount(); p++)
                            {
                                pName = nullptr;
                                pTypeId = 0;
                                t->GetProperty(p, &pName, &pTypeId);

                                if (pName && std::string(pName) == tok)
                                {
                                    decl = engine->GetTypeDeclaration(pTypeId, true);

                                    if (decl)
                                    {
                                        inferredType = decl;
                                        found = true;
                                        break;
                                    }
                                }
                            }

                            if (!found)
                            {
                                for (asUINT m = 0; m < t->GetMethodCount(); m++)
                                {
                                    func = t->GetMethodByIndex(m);

                                    if (func && std::string(func->GetName()) == tok)
                                    {
                                        pTypeId = func->GetReturnTypeId();
                                        decl = engine->GetTypeDeclaration(pTypeId, true);

                                        if (decl)
                                        {
                                            inferredType = decl;
                                            found = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!inferredType.empty())
            {
                inferredType.erase(std::remove(inferredType.begin(), inferredType.end(), '@'), inferredType.end());
                inferredType.erase(std::remove(inferredType.begin(), inferredType.end(), '&'), inferredType.end());
                return inferredType;
            }

            return "auto";
        }

        /**
         * @brief Evaluates statement configurations verifying whether tracking codes map to fresh variable allocations.
         * @param savedPos Stored backup positioning placeholder used to backtrack safely.
         * @param directlyInClass Flag confirming whether operations sit directly inside target object bodies templates.
         * @return True if a local variable signature validates, false otherwise.
         */
        bool ProcessLocalDeclaration(size_t savedPos, bool directlyInClass)
        {
            std::string parsedType = "";
            std::string baseType = "";
            std::string varName = "";
            std::string scopePrefix = "";
            std::string nature = "";
            asETokenClass tc = asTC_UNKNOWN;
            int effectiveDepth = 0;
            bool isLocal = false;

            stream.SetPos(savedPos);
            parsedType = ParseDataType(engine, stream, knownClassNames);

            if (parsedType.empty())
            {
                stream.SetPos(savedPos);
                return false;
            }

            baseType = GetBaseType(parsedType);
            if (!IsValidDataType(engine, stream, baseType, knownClassNames))
            {
                stream.SetPos(savedPos);
                return false;
            }

            tc = stream.Peek(token);

            if (tc != asTC_IDENTIFIER)
            {
                stream.SetPos(savedPos);
                return false;
            }

            varName = std::string(token);
            stream.Advance(token);
            tc = stream.Peek(token);

            if (tc == asTC_KEYWORD && (token == ";" || token == "=" || token == "," || token == ")" || token == "("))
            {
                isLocal = false;
                effectiveDepth = currentDepth;

                if (parenDepth > 0)
                {
                    isLocal = true;
                    effectiveDepth = currentDepth + 1;
                }
                else if (currentDepth > 0 && !directlyInClass)
                {
                    isLocal = true;
                }

                if (isLocal)
                {
                    auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable &v)
                                           { return v.name == varName; });

                    if (it == locals.end())
                    {
                        locals.push_back({varName, parsedType, effectiveDepth});
                        scopePrefix = GetFullScope().empty() ? "::" : GetFullScope() + "::";
                        nature = GetVariableNature(parenDepth, effectiveDepth);

                        HARVEST_DEBUG(fmt::format("NEW LOCAL VARIABLE: '{}{}' of type '{}' ({}) at depth {}",
                                                  scopePrefix, varName, parsedType, nature, effectiveDepth));
                    }

                    ParseAssignments(parsedType, effectiveDepth, varName);
                    return true;
                }
            }

            stream.SetPos(savedPos);
            return false;
        }

        /**
         * @brief Evaluates inline initialization assignments and comma-separated definition sequences.
         * @param parsedType Active foundational type description applied over current segments.
         * @param effectiveDepth Nested tracking depth assigned to context operations.
         * @param currentVarName Base tag labeling the active targeted variable item.
         */
        void ParseAssignments(std::string parsedType, int effectiveDepth, std::string currentVarName)
        {
            std::vector<std::string> expressionTokens;
            std::string inferred = "";
            std::string nextVar = "";
            std::string scopePrefix = "";
            std::string nature = "";
            asETokenClass lookTc = asTC_UNKNOWN;
            bool isAssigning = false;

            while (true)
            {
                if (stream.IsEOF())
                    break;
                lookTc = stream.Peek(token);

                if (lookTc == asTC_KEYWORD && token == "function")
                {
                    stream.Advance(token);
                    if (stream.Peek(token) == asTC_KEYWORD && token == "(")
                    {
                        stream.Advance(token);
                        while (!stream.IsEOF() && token != ")")
                        {
                            std::string paramType = ParseDataType(engine, stream, knownClassNames);
                            if (!paramType.empty() && stream.Peek(token) == asTC_IDENTIFIER)
                            {
                                std::string paramName = std::string(token);
                                stream.Advance(token);
                                locals.push_back({paramName, paramType, currentDepth + 1});
                                scopePrefix = GetFullScope().empty() ? "::" : GetFullScope() + "::";
                                HARVEST_DEBUG(fmt::format("NEW LOCAL VARIABLE: '{}{}' of type '{}' (lambda parameter) at depth {}",
                                                          scopePrefix, paramName, paramType, currentDepth + 1));
                            }
                            else
                            {
                                stream.Advance(token);
                            }
                            if (stream.Peek(token) == asTC_KEYWORD && token == ",")
                            {
                                stream.Advance(token);
                            }
                        }
                        if (stream.Peek(token) == asTC_KEYWORD && token == ")")
                        {
                            stream.Advance(token);
                        }
                    }
                    if (stream.Peek(token) == asTC_KEYWORD && token == "{")
                    {
                        break;
                    }
                    continue;
                }

                if (lookTc == asTC_UNKNOWN || (lookTc == asTC_KEYWORD && (token == ";" || token == ")")))
                {
                    if (isAssigning && parsedType.find("auto") != std::string::npos && !expressionTokens.empty())
                    {
                        inferred = InferAutoType(expressionTokens);

                        if (inferred != "auto")
                        {
                            parsedType = inferred;
                            auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable &v)
                                                   { return v.name == currentVarName; });

                            if (it != locals.end())
                            {
                                it->typeName = parsedType;
                            }
                        }
                    }

                    stream.Advance(token);

                    if (token == ")")
                    {
                        if (parenDepth > 0)
                        {
                            parenDepth--;
                        }

                        lastTokenStr = ")";
                    }
                    else
                    {
                        lastTokenStr = ";";
                    }

                    break;
                }

                if (lookTc == asTC_KEYWORD && token == "=")
                {
                    stream.Advance(token);
                    lastTokenStr = "=";
                    isAssigning = true;
                    expressionTokens.clear();
                    continue;
                }

                if (lookTc == asTC_KEYWORD && token == ",")
                {
                    if (parenDepth > 0)
                    {
                        break;
                    }

                    if (isAssigning && parsedType.find("auto") != std::string::npos && !expressionTokens.empty())
                    {
                        inferred = InferAutoType(expressionTokens);

                        if (inferred != "auto")
                        {
                            parsedType = inferred;
                            auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable &v)
                                                   { return v.name == currentVarName; });

                            if (it != locals.end())
                            {
                                it->typeName = parsedType;
                            }
                        }
                    }

                    stream.Advance(token);
                    lastTokenStr = ",";
                    isAssigning = false;
                    expressionTokens.clear();
                    stream.Peek(token);

                    if (IsValidDataType(engine, stream, token, knownClassNames))
                    {
                        break;
                    }

                    if (stream.Peek(token) == asTC_IDENTIFIER)
                    {
                        nextVar = std::string(token);
                        stream.Advance(token);
                        lastTokenStr = nextVar;
                        currentVarName = nextVar;

                        auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable &v)
                                               { return v.name == nextVar; });

                        if (it == locals.end())
                        {
                            locals.push_back({nextVar, parsedType, effectiveDepth});
                            scopePrefix = GetFullScope().empty() ? "::" : GetFullScope() + "::";
                            nature = GetVariableNature(parenDepth, effectiveDepth);

                            HARVEST_DEBUG(fmt::format("NEW LOCAL VARIABLE: '{}{}' of type '{}' ({}) at depth {}",
                                                      scopePrefix, nextVar, parsedType, nature, effectiveDepth));
                        }

                        continue;
                    }
                }
                else
                {
                    stream.Advance(token);

                    if (isAssigning && lookTc != asTC_WHITESPACE && lookTc != asTC_COMMENT)
                    {
                        expressionTokens.push_back(std::string(token));
                    }

                    if (token == "(")
                    {
                        parenDepth++;
                    }
                    else if (token == ")")
                    {
                        if (parenDepth > 0)
                        {
                            parenDepth--;
                        }
                    }

                    lastTokenStr = std::string(token);
                }
            }
        }
    };

    /**
     * @brief Public parsing entry tracking stack items available at particular coordinates locations.
     * @param engine Host compiler processing configuration context handle.
     * @param code View covering active document code stream layout data.
     * @param cursorAbsolutePos Absolute positioning character indicator marking targets coordinates boundaries.
     * @param outEnclosingClass Receives the name of the active surrounding class structure definition layer if found.
     * @param customClasses Mapped descriptors detailing custom script layout profiles.
     * @param globalVars Collection summarizing file layer variable properties.
     * @param globalFuncs Registry mapping globally compiled operations methods routines.
     * @return Listing aggregating variables identified as active at selection limits.
     */
    std::vector<LocalVariable> ScanLocalVariables(
        asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos, std::string &outEnclosingClass,
        const std::vector<ScriptClass> &customClasses, const std::vector<GlobalVariable> &globalVars, const std::vector<GlobalFunction> &globalFuncs)
    {
        LocalVariableScanner scanner(engine, code, cursorAbsolutePos, customClasses, globalVars, globalFuncs);
        return scanner.Scan(outEnclosingClass);
    }

    /**
     * @brief Performs sweeps identifying and indexing free functional methods configured over root layouts.
     * @param engine Target interface configuration platform backend pointer context.
     * @param code Absolute character string describing the input document source stream.
     * @return Vector listing global operations records isolated across global spaces.
     */
    std::vector<GlobalFunction> ScanGlobalFunctions(asIScriptEngine *engine, std::string_view code)
    {
        std::vector<GlobalFunction> funcs;
        std::vector<ScriptClass> customClasses;
        std::vector<std::string> localClasses;
        TokenStream stream(engine, code);
        std::string lastTokenStr = "";
        std::string parsedType = "";
        std::string funcName = "";
        std::string decl = "";
        std::string_view token;
        size_t savedPos = 0;
        size_t beforeTypePos = 0;
        asETokenClass tc = asTC_UNKNOWN;
        int currentDepth = 0;
        bool validDeclFound = false;

        customClasses = ScanCustomClasses(engine, code);

        for (const auto &c : customClasses)
        {
            localClasses.push_back(c.name);
        }

        while (!stream.IsEOF())
        {
            savedPos = stream.GetPos();
            tc = stream.Peek(token);

            if (tc == asTC_KEYWORD)
            {
                if (token == "{")
                {
                    currentDepth++;
                    stream.Advance(token);
                    lastTokenStr = "{";
                    continue;
                }

                if (token == "}")
                {
                    currentDepth--;
                    stream.Advance(token);
                    lastTokenStr = "}";
                    continue;
                }
            }

            while (tc == asTC_KEYWORD && IsGlobalModifier(token))
            {
                stream.Advance(token);
                lastTokenStr = std::string(token);
                tc = stream.Peek(token);
            }

            validDeclFound = false;

            if (lastTokenStr != "." && lastTokenStr != "->" && lastTokenStr != "::")
            {
                beforeTypePos = stream.GetPos();
                parsedType = ParseDataType(engine, stream, localClasses);

                if (!parsedType.empty() && stream.Peek(token) == asTC_IDENTIFIER)
                {
                    funcName = std::string(token);
                    stream.Advance(token);

                    if (stream.Peek(token) == asTC_KEYWORD && token == "(")
                    {
                        validDeclFound = true;

                        if (currentDepth == 0)
                        {
                            decl = fmt::format("{} {}()", parsedType, funcName);
                            auto it = std::find_if(funcs.begin(), funcs.end(), [&](const GlobalFunction &f)
                                                   { return f.name == funcName; });

                            if (it == funcs.end())
                            {
                                funcs.push_back({funcName, parsedType, decl});
                            }
                        }
                    }
                }

                if (!validDeclFound)
                {
                    stream.SetPos(beforeTypePos);
                }
            }

            if (!validDeclFound && stream.GetPos() == savedPos)
            {
                stream.Advance(token);
                lastTokenStr = std::string(token);
            }
        }

        return funcs;
    }

    /**
     * @brief Isolates standalone properties configured over primary root file levels layers.
     * @param engine Active application database connection frame runtime pointer context.
     * @param code Main view mapping flat text rows formatting current projects records code views.
     * @return Listing summarizing global variable metadata records isolated.
     */
    std::vector<GlobalVariable> ScanGlobalVariables(asIScriptEngine *engine, std::string_view code)
    {
        std::vector<GlobalVariable> vars;
        std::vector<ScriptClass> customClasses;
        std::vector<std::string> localClasses;
        TokenStream stream(engine, code);
        std::string lastTokenStr = "";
        std::string parsedType = "";
        std::string varName = "";
        std::string nextVar = "";
        std::string_view token;
        size_t savedPos = 0;
        size_t beforeTypePos = 0;
        asETokenClass tc = asTC_UNKNOWN;
        asETokenClass lookTc = asTC_UNKNOWN;
        int currentDepth = 0;
        bool validDeclFound = false;

        customClasses = ScanCustomClasses(engine, code);

        for (const auto &c : customClasses)
        {
            localClasses.push_back(c.name);
        }

        while (!stream.IsEOF())
        {
            savedPos = stream.GetPos();
            tc = stream.Peek(token);

            if (tc == asTC_KEYWORD)
            {
                if (token == "{")
                {
                    currentDepth++;
                    stream.Advance(token);
                    lastTokenStr = "{";
                    continue;
                }

                if (token == "}")
                {
                    currentDepth--;
                    stream.Advance(token);
                    lastTokenStr = "}";
                    continue;
                }
            }

            while (tc == asTC_KEYWORD && IsGlobalModifier(token))
            {
                stream.Advance(token);
                lastTokenStr = std::string(token);
                tc = stream.Peek(token);
            }

            validDeclFound = false;

            if (lastTokenStr != "." && lastTokenStr != "->" && lastTokenStr != "::")
            {
                beforeTypePos = stream.GetPos();
                parsedType = ParseDataType(engine, stream, localClasses);

                if (!parsedType.empty() && stream.Peek(token) == asTC_IDENTIFIER)
                {
                    varName = std::string(token);
                    stream.Advance(token);
                    tc = stream.Peek(token);

                    if (tc == asTC_KEYWORD && IsPropertySeparator(token))
                    {
                        validDeclFound = true;

                        if (currentDepth == 0)
                        {
                            auto it = std::find_if(vars.begin(), vars.end(), [&](const GlobalVariable &v)
                                                   { return v.name == varName; });

                            if (it == vars.end())
                            {
                                vars.push_back({varName, parsedType});
                            }

                            while (true)
                            {
                                lookTc = stream.Peek(token);

                                if (lookTc == asTC_UNKNOWN || (lookTc == asTC_KEYWORD && token == ";"))
                                {
                                    break;
                                }

                                if (lookTc == asTC_KEYWORD && token == ",")
                                {
                                    stream.Advance(token);
                                    lastTokenStr = ",";

                                    if (stream.Peek(token) == asTC_IDENTIFIER)
                                    {
                                        nextVar = std::string(token);
                                        stream.Advance(token);
                                        lastTokenStr = nextVar;
                                        it = std::find_if(vars.begin(), vars.end(), [&](const GlobalVariable &v)
                                                          { return v.name == nextVar; });

                                        if (it == vars.end())
                                        {
                                            vars.push_back({nextVar, parsedType});
                                        }

                                        continue;
                                    }
                                }
                                else
                                {
                                    stream.Advance(token);
                                    lastTokenStr = std::string(token);
                                }
                            }
                        }
                    }
                }

                if (!validDeclFound)
                {
                    stream.SetPos(beforeTypePos);
                }
            }

            if (!validDeclFound && stream.GetPos() == savedPos)
            {
                stream.Advance(token);
                lastTokenStr = std::string(token);
            }
        }

        return vars;
    }
} // namespace TokenHarvester