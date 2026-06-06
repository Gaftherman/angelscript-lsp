/**
 * @file TokenHarvester.cpp
 * @brief Implements fault-tolerant EBNF-guided grammar scanning for AngelScript LSP.
 */

#include "TokenHarvester.h"
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
     * @return True if it is a class, interface, namespace, enum, or mixin.
     */
    static inline bool IsTypeDeclaration(std::string_view token)
    {
        return token == "class" || token == "interface" || token == "namespace" || token == "enum" || token == "mixin";
    }

    /**
     * @brief Identifies if a token is an access or sharing modifier.
     * @param token The keyword string to evaluate.
     * @return True if modifies scope visibility or shared status.
     */
    static inline bool IsAccessModifier(std::string_view token)
    {
        return token == "private" || token == "protected" || token == "public" || token == "shared";
    }

    /**
     * @brief Identifies if a token is a global structural modifier.
     * @param token The keyword string to evaluate.
     * @return True if it is an access modifier, external, or mixin declaration.
     */
    static inline bool IsGlobalModifier(std::string_view token)
    {
        return IsAccessModifier(token) || token == "external" || token == "mixin";
    }

    /**
     * @brief Identifies if a token acts as an end-of-statement or property separator.
     * @param token The keyword string to evaluate.
     * @return True if it is a semicolon, comma, or assignment operator.
     */
    static inline bool IsPropertySeparator(std::string_view token)
    {
        return token == ";" || token == "=" || token == ",";
    }

    /**
     * @brief Identifies if a token represents a core built-in data type or native template.
     * @param token The keyword string to evaluate.
     * @return True if it is a primitive type (int, float, etc.) or a native template (array, dictionary).
     */
    static inline bool IsBuiltInType(std::string_view token)
    {
        return token == "void" || token == "int" || token == "int8" || token == "int16" || token == "int32" || token == "int64" ||
               token == "uint" || token == "uint8" || token == "uint16" || token == "uint32" || token == "uint64" ||
               token == "float" || token == "double" || token == "bool" || token == "auto" || token == "string" ||
               token == "array" || token == "dictionary" || token == "grid";
    }

    /**
     * @brief Identifies if a token is a method declaration modifier.
     * @param token The keyword string to evaluate.
     * @return True if the token is a standard method modifier (const, override, final, property).
     */
    static inline bool IsMethodModifier(std::string_view token)
    {
        return token == "const" || token == "override" || token == "final" || token == "property";
    }

    /**
     * @brief Identifies if a token is a formal EBNF control flow keyword that opens statement blocks.
     * @param token The keyword string to evaluate.
     * @return True if it is a control flow statement introducer (if, for, while, switch, etc.).
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
         */
        TokenStream(asIScriptEngine *e, std::string_view c, size_t start = 0)
            : engine(e), code(c), pos(start) {}

        /**
         * @brief Peeks at the upcoming token class without advancing the stream position.
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
         */
        bool IsEOF()
        {
            std::string_view dummy;
            asETokenClass tc = Peek(dummy);
            return (tc == asTC_UNKNOWN && dummy.empty());
        }

        /**
         * @brief Gets the current internal buffer position.
         */
        size_t GetPos() const { return pos; }

        /**
         * @brief Sets the internal buffer position.
         */
        void SetPos(size_t p) { pos = p; }
    };

    /**
     * @brief Resolves and extracts the primitive base type from modifiers or templates.
     */
    std::string GetBaseType(const std::string &type)
    {
        std::string clean = type;
        size_t start = 0;
        size_t templatePos = std::string::npos;

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
        templatePos = clean.find('<');

        if (templatePos != std::string::npos)
        {
            clean = clean.substr(0, templatePos);
        }

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

        if (clean.find("[]") != std::string::npos)
        {
            return "array";
        }

        return clean;
    }

    /**
     * @brief Extracts the underlying parameter type wrapped inside native structural collections.
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
     */
    static bool IsValidDataType(asIScriptEngine *engine, TokenStream &stream, std::string_view token, const std::vector<std::string> &localClasses)
    {
        std::string tokenStr(token);
        size_t savedPos = 0;
        std::string_view dummy;

        if (IsBuiltInType(token))
        {
            return true;
        }

        if (engine->GetTypeInfoByName(tokenStr.c_str()) != nullptr)
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

            while (templateDepth > 0 && stream.Peek(token) != asTC_UNKNOWN)
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
     * @brief Translates localized grid metrics into an absolute memory position index.
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
     * @brief Evaluates code scope context preceding the active user cursor point.
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

            if (!lastTok.empty() && (isalpha(lastTok[0]) || lastTok[0] == '_'))
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

                    if (!tok.empty() && (isalpha(tok[0]) || tok[0] == '_' || tok == "this" || tok == "super"))
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
     * @brief Base structural scanner mapping program scopes.
     */
    class ASTScanner
    {
    protected:
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
         * @brief Constructs a new ASTScanner instance.
         */
        ASTScanner(asIScriptEngine *e, std::string_view c)
            : engine(e), code(c), stream(e, c), currentDepth(0) {}

    protected:
        /**
         * @brief Resolves fully qualified scope path prefixes based on current context stack.
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
         * @brief Advances stream forward skipping localized statement or routine code blocks.
         */
        void SkipBlock()
        {
            int methodDepth = 1;

            while (methodDepth > 0 && stream.Peek(token) != asTC_UNKNOWN)
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
         * @brief Builds tracking collections matching visible custom structured types.
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
     * @brief Specialized grammar scanner classifying structure and class layouts.
     */
    class CustomClassScanner : public ASTScanner
    {
    private:
        std::map<std::string, ScriptClass> classMap;
        std::string currentAccess;

    public:
        /**
         * @brief Constructs a new Custom Class Scanner object.
         */
        CustomClassScanner(asIScriptEngine *e, std::string_view c) : ASTScanner(e, c), currentAccess("public")
        {
            CollectKnownTypes();
        }

        /**
         * @brief Parses the active stream context identifying nested type definitions.
         */
        std::vector<ScriptClass> Scan()
        {
            std::vector<std::string> bases;
            std::string fullScope = "";
            std::vector<ScriptClass> results;
            size_t savedPos = 0;
            asETokenClass tc = asTC_UNKNOWN;
            bool validDeclFound = false;

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

                            while (stream.Peek(token) != asTC_UNKNOWN && token != "{")
                            {
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

                validDeclFound = ProcessConstructorOrDestructor(tc);

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
         * @brief Maps parsed identifiers into enum values inside structured scopes.
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
         * @brief Evaluates signatures targeting constructor or destructor procedures.
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
         * @brief Advances stream skip patterns directly up to the execution block context.
         */
        void SkipToBody()
        {
            while (stream.Peek(token) != asTC_UNKNOWN && token != "{" && token != ";")
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
         * @brief Differentiates and structures fields versus nested routine declarations.
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
         * @brief Extracts parameters and processes explicit class method metadata.
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

            } while (parenDepth > 0 && stream.Peek(token) != asTC_UNKNOWN);

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
         * @brief Registers object instances or field parameters into active class contexts.
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
     * @brief High-level orchestration mapping object layouts discovered in raw context buffers.
     */
    std::vector<ScriptClass> ScanCustomClasses(asIScriptEngine *engine, std::string_view code)
    {
        CustomClassScanner scanner(engine, code);
        return scanner.Scan();
    }

    /**
     * @class LocalVariableScanner
     * @brief Formally structured state machine mapping variables matching explicit EBNF blocks.
     */
    class LocalVariableScanner : public ASTScanner
    {
    private:
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
         * @brief Constructs a new Local Variable Scanner instance.
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
         * @brief Determines environmental source characteristics framing a localized variable entry.
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
         * @brief Scans statement tracking windows classifying declarations up to the cursor position.
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
            int captureDepth = 0;
            bool directlyInClass = false;
            bool validDeclFound = false;
            bool insideClassDecl = false;

            while (stream.GetPos() < cursorAbsolutePos && !stream.IsEOF())
            {
                savedPos = stream.GetPos();
                tc = stream.Peek(token);

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

                            while (stream.Peek(token) != asTC_UNKNOWN && token != ")")
                            {
                                stream.Advance(token);

                                if (token == ":")
                                {
                                    while (stream.Peek(token) != asTC_UNKNOWN && token != ")")
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
         * @brief Deduces static datatypes assigned into dynamically resolved keywords.
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

            HARVEST_DEBUG(fmt::format("Evaluating 'auto' with expression: {}", tokens[0]));

            if (tokens[0][0] == '"' || tokens[0][0] == '\'')
            {
                return "string";
            }

            if (tokens[0] == "true" || tokens[0] == "false")
            {
                return "bool";
            }

            if (tokens[0].find_first_of(".f") != std::string::npos && isdigit(tokens[0][0]))
            {
                return "float";
            }

            if (isdigit(tokens[0][0]))
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
                        HARVEST_DEBUG(fmt::format("Detected bracket [ in type: '{}'", inferredType));
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

                if (!isalpha(tok[0]) && tok[0] != '_')
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

                        HARVEST_DEBUG(fmt::format("Expression root '{}' resolved as: '{}'", tok, inferredType));
                    }
                }
                else
                {
                    baseType = GetBaseType(inferredType);
                    found = false;
                    HARVEST_DEBUG(fmt::format("Searching for member '{}' inside '{}'", tok, baseType));

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
                HARVEST_DEBUG(fmt::format("Final inferred auto: '{}'", inferredType));
                return inferredType;
            }

            HARVEST_DEBUG("Failed to infer auto. Returning 'auto'");
            return "auto";
        }

        /**
         * @brief Evaluates statements assessing presence of inline standard variables.
         */
        bool ProcessLocalDeclaration(size_t savedPos, bool directlyInClass = false)
        {
            std::string parsedType = "";
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
         * @brief Evaluates assignment steps modifying structural metadata of mapped instances.
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
                lookTc = stream.Peek(token);

                if (lookTc == asTC_UNKNOWN || (lookTc == asTC_KEYWORD && (token == ";" || token == ")")))
                {
                    if (isAssigning && parsedType == "auto" && !expressionTokens.empty())
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
                    if (isAssigning && parsedType == "auto" && !expressionTokens.empty())
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
     * @brief Formally scans script segments constructing visible list indices tracking local states.
     */
    std::vector<LocalVariable> ScanLocalVariables(
        asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos, std::string &outEnclosingClass,
        const std::vector<ScriptClass> &customClasses, const std::vector<GlobalVariable> &globalVars, const std::vector<GlobalFunction> &globalFuncs)
    {
        LocalVariableScanner scanner(engine, code, cursorAbsolutePos, customClasses, globalVars, globalFuncs);
        return scanner.Scan(outEnclosingClass);
    }

    /**
     * @brief Parses an un-scoped script window for publicly accessible global routines.
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
     * @brief Parses file blocks capturing variable setups defined at the outer global scope layer.
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