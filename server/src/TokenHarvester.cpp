/**
 * @file TokenHarvester.cpp
 * @brief Implements fault-tolerant scanning for AngelScript LSP.
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

/**
 * @namespace TokenHarvester
 * @brief Provides structures and functions for token parsing, scoping, and semantic analysis.
 */
namespace TokenHarvester
{
    /**
     * @brief Identifies if a token represents a scope or type declaration block.
     * @param token The keyword string to evaluate.
     * @return True if it is a class, interface, namespace, or enum.
     */
    static inline bool IsTypeDeclaration(std::string_view token)
    {
        return token == "class" || token == "interface" || token == "namespace" || token == "enum";
    }

    /**
     * @brief Identifies if a token is an access or sharing modifier.
     * @param token The keyword string to evaluate.
     * @return True if it modifies scope visibility or shared status.
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
     * @return True if the token is a standard method modifier (const, override, final).
     */
    static inline bool IsMethodModifier(std::string_view token)
    {
        return token == "const" || token == "override" || token == "final";
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
         * @brief Constructs a TokenStream instance.
         * @param e Pointer to the active AngelScript engine.
         * @param c The source code string view to be tokenized.
         * @param start The initial absolute parsing position.
         */
        TokenStream(asIScriptEngine *e, std::string_view c, size_t start = 0)
            : engine(e), code(c), pos(start) {}

        /**
         * @brief Inspects the next token without advancing the stream position.
         * @param outToken Reference to a string_view that will hold the extracted token.
         * @return The AngelScript token class type of the peeked token.
         */
        asETokenClass Peek(std::string_view &outToken)
        {
            size_t tempPos = pos;
            asUINT len = 0;
            asETokenClass tc;

            do
            {
                if (tempPos >= code.length())
                    return asTC_UNKNOWN;
                tc = engine->ParseToken(code.data() + tempPos, static_cast<asUINT>(code.length() - tempPos), &len);
                if (len == 0)
                    return asTC_UNKNOWN;
                outToken = code.substr(tempPos, len);
                tempPos += len;
            } while (tc == asTC_WHITESPACE || tc == asTC_COMMENT);

            return tc;
        }

        /**
         * @brief Consumes and returns the next token, advancing the stream position.
         * @param outToken Reference to a string_view that will hold the extracted token.
         * @return The AngelScript token class type of the advanced token.
         */
        asETokenClass Advance(std::string_view &outToken)
        {
            asUINT len = 0;
            asETokenClass tc;

            do
            {
                if (pos >= code.length())
                    return asTC_UNKNOWN;
                tc = engine->ParseToken(code.data() + pos, static_cast<asUINT>(code.length() - pos), &len);
                if (len == 0)
                    return asTC_UNKNOWN;
                outToken = code.substr(pos, len);
                pos += len;
            } while (tc == asTC_WHITESPACE || tc == asTC_COMMENT);

            return tc;
        }

        /**
         * @brief Retrieves the current absolute position within the source code.
         * @return The absolute character index.
         */
        size_t GetPos() const { return pos; }

        /**
         * @brief Sets the absolute position within the source code.
         * @param p The new absolute character index.
         */
        void SetPos(size_t p) { pos = p; }
    };

    /**
     * @brief Extracts the core base type from a complex type declaration string.
     * @param type The raw type declaration.
     * @return A simplified string representing the base type.
     */
    std::string GetBaseType(const std::string &type)
    {
        std::string clean = type;

        size_t start = 0;
        while (start < clean.length() && clean[start] == ' ')
            start++;
        if (clean.substr(start, 6) == "const ")
            start += 6;
        while (start < clean.length() && clean[start] == ' ')
            start++;
        clean = clean.substr(start);

        size_t templatePos = clean.find('<');
        if (templatePos != std::string::npos)
            clean = clean.substr(0, templatePos);

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
            return "array";

        return clean;
    }

    /**
     * @brief Extracts the inner generic or array type.
     * @param type The wrapped type declaration (e.g., array<int>).
     * @return The inner type string.
     */
    std::string ExtractInnerType(const std::string &type)
    {
        size_t start = type.find('<');
        size_t end = type.rfind('>');
        if (start != std::string::npos && end != std::string::npos && end > start)
        {
            return type.substr(start + 1, end - start - 1);
        }
        size_t bracket = type.rfind("[]");
        if (bracket != std::string::npos)
        {
            return type.substr(0, bracket);
        }
        return type;
    }

    /**
     * @brief Simplifies an instantiated type string by removing qualifiers and handles.
     * @param type The raw type declaration.
     * @return The cleaned instantiated type string.
     */
    std::string GetInstantiatedType(const std::string &type)
    {
        std::string clean = type;

        size_t start = 0;
        while (start < clean.length() && clean[start] == ' ')
            start++;
        if (clean.substr(start, 6) == "const ")
            start += 6;
        while (start < clean.length() && clean[start] == ' ')
            start++;
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
     * @brief Validates whether a token sequence represents a known, legitimate data type.
     * @param engine Pointer to the active AngelScript engine.
     * @param stream The current token stream context.
     * @param token The initial token to evaluate and validate.
     * @param localClasses A vector of locally defined class names.
     * @return True if the token resolves to a valid type; otherwise, false.
     */
    static bool IsValidDataType(asIScriptEngine *engine, TokenStream &stream, std::string_view token, const std::vector<std::string> &localClasses)
    {
        if (IsBuiltInType(token))
            return true;

        std::string tokenStr(token);
        if (engine->GetTypeInfoByName(tokenStr.c_str()) != nullptr)
            return true;
        if (std::find(localClasses.begin(), localClasses.end(), tokenStr) != localClasses.end())
            return true;

        size_t savedPos = stream.GetPos();
        std::string_view dummy;
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
     * @brief Parses and constructs a complete data type string from the token stream.
     * @param engine Pointer to the active AngelScript engine.
     * @param stream The active token stream to parse from.
     * @param knownClassNames A vector of locally defined known class names.
     * @return The fully reconstructed data type string.
     */
    static std::string ParseDataType(asIScriptEngine *engine, TokenStream &stream, const std::vector<std::string> &knownClassNames)
    {
        std::string typeName = "";
        std::string_view token;
        asETokenClass tc = stream.Peek(token);

        if (tc == asTC_KEYWORD && token == "const")
        {
            typeName += "const ";
            stream.Advance(token);
            tc = stream.Peek(token);
        }

        if (tc == asTC_KEYWORD || tc == asTC_IDENTIFIER)
        {
            typeName += std::string(token);
            stream.Advance(token);
            tc = stream.Peek(token);
        }
        else
        {
            return "";
        }

        if (tc == asTC_KEYWORD && token == "<")
        {
            typeName += "<";
            stream.Advance(token);
            int templateDepth = 1;
            while (templateDepth > 0 && stream.Peek(token) != asTC_UNKNOWN)
            {
                tc = stream.Peek(token);
                if (tc == asTC_KEYWORD && token == "<")
                    templateDepth++;
                else if (tc == asTC_KEYWORD && token == ">")
                    templateDepth--;

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
     * @brief Calculates the linear, zero-based character index from line and column offsets.
     * @param text The full source code string view.
     * @param line The target line number (zero-based).
     * @param character The character column on the target line.
     * @return The absolute character position in the buffer.
     */
    size_t GetAbsolutePosition(std::string_view text, int line, int character)
    {
        size_t pos = 0;
        int currentLine = 0;
        while (currentLine < line && pos < text.length())
        {
            if (text[pos] == '\n')
                currentLine++;
            pos++;
        }
        return pos + character;
    }

    /**
     * @brief Evaluates the source context prior to the cursor to build a completion state object.
     * @param engine Pointer to the active AngelScript engine.
     * @param code The full source code string view.
     * @param cursorAbsolutePos The absolute position of the user's cursor.
     * @return A CompletionContext object containing structural context for language servers.
     */
    CompletionContext GetCompletionContext(asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos)
    {
        CompletionContext ctx;
        ctx.isMemberAccess = false;
        ctx.partialMember = "";
        ctx.lastSeparator = "";

        TokenStream stream(engine, code);
        std::string_view token;
        std::vector<std::string> tokens;

        while (stream.GetPos() < cursorAbsolutePos && stream.Peek(token) != asTC_UNKNOWN)
        {
            stream.Advance(token);
            tokens.push_back(std::string(token));
        }

        if (tokens.empty())
            return ctx;

        int i = (int)tokens.size() - 1;

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

        if (ctx.isMemberAccess)
        {
            std::vector<std::string> chain;
            while (i >= 0)
            {
                std::string fullIdent = "";

                while (i >= 0 && (tokens[i] == "]" || tokens[i] == ")"))
                {
                    std::string closingToken = tokens[i];
                    std::string openingToken = (closingToken == "]") ? "[" : "(";
                    int depth = 0;
                    std::string segment = "";
                    do
                    {
                        if (tokens[i] == closingToken)
                            depth++;
                        else if (tokens[i] == openingToken)
                            depth--;
                        segment = tokens[i] + segment;
                        i--;
                    } while (i >= 0 && depth > 0);
                    fullIdent = segment + fullIdent;
                }

                if (i >= 0 && tokens[i] != "." && tokens[i] != "::")
                {
                    fullIdent = tokens[i] + fullIdent;
                    chain.insert(chain.begin(), fullIdent);
                    i--;
                }
                else if (!fullIdent.empty())
                {
                    chain.insert(chain.begin(), fullIdent);
                }

                if (i >= 0 && (tokens[i] == "." || tokens[i] == "::"))
                    i--;
                else
                    break;
            }
            ctx.objectChain = chain;
        }

        return ctx;
    }

    /**
     * @class ASTScanner
     * @brief Internal base state machine for parsing classes and scopes from tokens.
     */
    class ASTScanner
    {
    protected:
        /**
         * @struct ScopeCtx
         * @brief Represents a hierarchical block scope during lexical scanning.
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
         * @brief Constructs an ASTScanner.
         * @param e Pointer to the active AngelScript engine.
         * @param c The source code string view to parse.
         */
        ASTScanner(asIScriptEngine *e, std::string_view c)
            : engine(e), code(c), stream(e, c), currentDepth(0) {}

    protected:
        /**
         * @brief Computes the fully resolved namespace/class path from the active scope stack.
         * @return A string representing the absolute hierarchical scope.
         */
        std::string GetFullScope() const
        {
            std::string res;
            for (const auto &s : scopeStack)
            {
                if (!res.empty())
                    res += "::";
                res += s.name;
            }
            return res;
        }

        /**
         * @brief Consumes and ignores all tokens inside the current brace block.
         */
        void SkipBlock()
        {
            int methodDepth = 1;
            while (methodDepth > 0 && stream.Peek(token) != asTC_UNKNOWN)
            {
                stream.Advance(token);
                if (token == "{")
                    methodDepth++;
                else if (token == "}")
                    methodDepth--;
            }
        }

        /**
         * @brief Performs a fast preliminary pass to cache all user-defined class and scope names.
         */
        void CollectKnownTypes()
        {
            TokenStream quickStream(engine, code);
            std::vector<std::string> quickScope;
            int qDepth = 0;
            std::string qPendingName, qPendingType;

            while (quickStream.Peek(token) != asTC_UNKNOWN)
            {
                asETokenClass tc = quickStream.Advance(token);
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
                        std::string fScope = "";
                        for (auto &s : quickScope)
                            fScope += (fScope.empty() ? "" : "::") + s;
                        knownClassNames.push_back(fScope);
                        qPendingName = "";
                    }
                }
                else if (tc == asTC_KEYWORD && token == "}")
                {
                    if (!quickScope.empty())
                        quickScope.pop_back();
                    qDepth--;
                }
            }
        }
    };

    /**
     * @class CustomClassScanner
     * @brief Extends ASTScanner to extract full definitions of user-defined types.
     */
    class CustomClassScanner : public ASTScanner
    {
    private:
        std::map<std::string, ScriptClass> classMap;
        std::string currentAccess;

    public:
        /**
         * @brief Constructs a CustomClassScanner and pre-caches local type names.
         * @param e Pointer to the active AngelScript engine.
         * @param c The source code string view.
         */
        CustomClassScanner(asIScriptEngine *e, std::string_view c) : ASTScanner(e, c), currentAccess("public")
        {
            CollectKnownTypes();
        }

        /**
         * @brief Scans the text and collects completely parsed custom classes.
         * @return A vector of constructed ScriptClass definition objects.
         */
        std::vector<ScriptClass> Scan()
        {
            while (stream.Peek(token) != asTC_UNKNOWN)
            {
                size_t savedPos = stream.GetPos();
                asETokenClass tc = stream.Advance(token);

                if (tc == asTC_KEYWORD && IsTypeDeclaration(token))
                {
                    pendingScopeType = std::string(token);
                    if (stream.Peek(token) == asTC_IDENTIFIER)
                    {
                        pendingScopeName = std::string(token);
                        stream.Advance(token);

                        std::vector<std::string> bases;
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
                            std::string fullScope = GetFullScope();
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
                        std::string fullScope = GetFullScope();
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
                        currentAccess = std::string(token);
                    continue;
                }

                if (scopeStack.empty())
                    continue;

                if (ProcessEnum(tc))
                    continue;

                bool validDeclFound = ProcessConstructorOrDestructor(tc);
                if (!validDeclFound)
                {
                    validDeclFound = ProcessMember(savedPos);
                }

                if (validDeclFound)
                    currentAccess = "public";
                else if (stream.GetPos() == savedPos)
                    stream.Advance(token);
            }

            std::vector<ScriptClass> results;
            for (auto &pair : classMap)
                results.push_back(pair.second);
            return results;
        }

    private:
        /**
         * @brief Evaluates tokens specific to enumeration parsing.
         * @param tc The token class class being evaluated.
         * @return True if handled as an enumeration member.
         */
        bool ProcessEnum(asETokenClass tc)
        {
            if (scopeStack.back().type != "enum")
                return false;
            if (tc == asTC_IDENTIFIER)
            {
                classMap[GetFullScope()].properties.push_back({std::string(token), "int", "public"});
            }
            return true;
        }

        /**
         * @brief Scans and parses class constructors or destructors.
         * @param tc The token class being evaluated.
         * @return True if a constructor or destructor was processed.
         */
        bool ProcessConstructorOrDestructor(asETokenClass tc)
        {
            ScopeCtx &currentScope = scopeStack.back();
            std::string fullScope = GetFullScope();

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
         * @brief Fast-forwards the stream to the end of a method body or statement.
         */
        void SkipToBody()
        {
            while (stream.Peek(token) != asTC_UNKNOWN && token != "{" && token != ";")
                stream.Advance(token);
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
         * @brief Differentiates and parses standard class members (properties vs methods).
         * @param savedPos The position marker to rollback to upon failure.
         * @return True if a valid property or method was processed.
         */
        bool ProcessMember(size_t savedPos)
        {
            stream.SetPos(savedPos);
            std::string parsedType = ParseDataType(engine, stream, knownClassNames);
            if (parsedType.empty() || stream.Peek(token) != asTC_IDENTIFIER)
            {
                stream.SetPos(savedPos);
                return false;
            }

            std::string memberName = std::string(token);
            stream.Advance(token);
            asETokenClass lookTc = stream.Peek(token);

            if (lookTc == asTC_KEYWORD && token == "(")
            {
                ParseMethod(parsedType, memberName);
                return true;
            }
            else if (lookTc == asTC_KEYWORD && IsPropertySeparator(token))
            {
                ParseProperty(parsedType, memberName);
                return true;
            }
            else if (lookTc == asTC_KEYWORD && token == "{")
            {
                std::string fullScope = GetFullScope();
                classMap[fullScope].properties.push_back({memberName, parsedType, currentAccess});
                stream.Advance(token);
                SkipBlock();
                return true;
            }
            stream.SetPos(savedPos);
            return false;
        }

        /**
         * @brief Ingests an entire method signature and records it on the current class scope.
         * @param parsedType The return type of the method.
         * @param memberName The name identifier of the method.
         */
        void ParseMethod(const std::string &parsedType, const std::string &memberName)
        {
            std::string fullScope = GetFullScope();
            size_t sigStart = stream.GetPos();
            int parenDepth = 0;

            do
            {
                stream.Advance(token);
                if (token == "(")
                    parenDepth++;
                else if (token == ")")
                    parenDepth--;
            } while (parenDepth > 0 && stream.Peek(token) != asTC_UNKNOWN);

            size_t sigEnd = stream.GetPos();
            std::string exactParams(code.data() + sigStart, sigEnd - sigStart);
            std::string signature = parsedType + " " + fullScope + "::" + memberName + exactParams;

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
         * @brief Parses field properties and registers them on the current class scope.
         * @param parsedType The data type for the properties.
         * @param memberName The primary property name identifier.
         */
        void ParseProperty(const std::string &parsedType, const std::string &memberName)
        {
            std::string fullScope = GetFullScope();
            classMap[fullScope].properties.push_back({memberName, parsedType, currentAccess});

            while (true)
            {
                asETokenClass tcc = stream.Peek(token);
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
                        std::string nextVar = std::string(token);
                        stream.Advance(token);
                        classMap[fullScope].properties.push_back({nextVar, parsedType, currentAccess});
                        continue;
                    }
                }
                else
                    stream.Advance(token);
            }
        }
    };

    /**
     * @brief Discovers and catalogs completely defined custom classes inside the script buffer.
     * @param engine Pointer to the active AngelScript engine.
     * @param code The source code string view.
     * @return A vector of extracted custom ScriptClass representations.
     */
    std::vector<ScriptClass> ScanCustomClasses(asIScriptEngine *engine, std::string_view code)
    {
        CustomClassScanner scanner(engine, code);
        return scanner.Scan();
    }

    /**
     * @class LocalVariableScanner
     * @brief Extends ASTScanner to extract variables mapped to functional block scopes, featuring type inference for the 'auto' keyword.
     */
    class LocalVariableScanner : public ASTScanner
    {
    private:
        std::vector<LocalVariable> locals;
        int parenDepth;
        std::string lastTokenStr;
        size_t cursorAbsolutePos;

        const std::vector<ScriptClass> &customClasses;
        const std::vector<GlobalVariable> &globalVars;
        const std::vector<GlobalFunction> &globalFuncs;

    public:
        /**
         * @brief Constructs a LocalVariableScanner initialized with system and user dependencies.
         * @param e Pointer to the active AngelScript engine.
         * @param c The source code string view.
         * @param pos The absolute byte offset of the target scan window.
         * @param cClasses Known local custom class definitions.
         * @param gVars Known active global variables.
         * @param gFuncs Known active global functions.
         */
        LocalVariableScanner(asIScriptEngine *e, std::string_view c, size_t pos,
                             const std::vector<ScriptClass> &cClasses,
                             const std::vector<GlobalVariable> &gVars,
                             const std::vector<GlobalFunction> &gFuncs)
            : ASTScanner(e, c), parenDepth(0), cursorAbsolutePos(pos),
              customClasses(cClasses), globalVars(gVars), globalFuncs(gFuncs)
        {
            CollectKnownTypes();
        }

        /**
         * @brief Scans for block-scoped locals resolving upward from the set absolute cursor position.
         * @param outEnclosingClass Receives the name of the containing scope class, if applicable.
         * @return A vector of locally accessible variables.
         */
        std::vector<LocalVariable> Scan(std::string &outEnclosingClass)
        {
            while (stream.GetPos() < cursorAbsolutePos && stream.Peek(token) != asTC_UNKNOWN)
            {
                size_t savedPos = stream.GetPos();
                asETokenClass tc = stream.Peek(token);

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
                    if (!scopeStack.empty() && scopeStack.back().depth == currentDepth)
                    {
                        scopeStack.pop_back();
                    }
                    currentDepth--;

                    int captureDepth = currentDepth;
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

                        size_t foreachPos = stream.GetPos();
                        std::string foreachType = ParseDataType(engine, stream, knownClassNames);

                        if (!foreachType.empty() && stream.Peek(token) == asTC_IDENTIFIER)
                        {
                            std::string loopVar = std::string(token);
                            stream.Advance(token);

                            std::vector<std::string> containerExpr;
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
                                std::string containerType = InferAutoType(containerExpr);
                                std::string inner = ExtractInnerType(containerType);
                                if (!inner.empty() && inner != containerType)
                                {
                                    if (foreachType.back() == '@' && inner.back() != '@')
                                        foreachType = inner + "@";
                                    else
                                        foreachType = inner;
                                }
                            }

                            locals.push_back({loopVar, foreachType, currentDepth + 1});
                            HARVEST_DEBUG(fmt::format("NUEVA VARIABLE FOREACH: '{}' de tipo '{}'", loopVar, foreachType));
                        }
                        else
                            stream.SetPos(foreachPos);
                    }
                    continue;
                }

                while (tc == asTC_KEYWORD && IsAccessModifier(token))
                {
                    stream.Advance(token);
                    lastTokenStr = std::string(token);
                    tc = stream.Peek(token);
                }

                bool directlyInClass = false;
                if (!scopeStack.empty() && scopeStack.back().type == "class" && currentDepth == scopeStack.back().depth)
                {
                    directlyInClass = true;
                }

                bool validDeclFound = false;
                if (lastTokenStr != "." && lastTokenStr != "->" && lastTokenStr != "::")
                {
                    validDeclFound = ProcessLocalDeclaration(savedPos, directlyInClass);
                }

                if (!validDeclFound && stream.GetPos() == savedPos)
                {
                    stream.Advance(token);
                    if (token == "(")
                        parenDepth++;
                    else if (token == ")")
                    {
                        if (parenDepth > 0)
                            parenDepth--;
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
         * @brief Acts as a mini-evaluator, executing assignment expressions contextually to deduce the true type of 'auto' variables.
         * @param tokens The ordered stream of tokens representing the initialization expression.
         * @return The deduced concrete type name as a string, or 'auto' if inference failed.
         */
        std::string InferAutoType(const std::vector<std::string> &tokens)
        {
            if (tokens.empty())
                return "auto";

            HARVEST_DEBUG(fmt::format("Evaluando 'auto' con la expresion: {}", tokens[0]));

            if (tokens[0][0] == '"' || tokens[0][0] == '\'')
                return "string";
            if (tokens[0] == "true" || tokens[0] == "false")
                return "bool";
            if (tokens[0].find_first_of(".f") != std::string::npos && isdigit(tokens[0][0]))
                return "float";
            if (isdigit(tokens[0][0]))
                return "int";

            std::string inferredType = "";
            std::string currentEnclosingClass = GetFullScope();
            int tempBracketDepth = 0;
            int tempParenDepth = 0;

            for (size_t i = 0; i < tokens.size(); i++)
            {
                std::string tok = tokens[i];

                if (tok == "[")
                {
                    if (tempBracketDepth == 0)
                    {
                        HARVEST_DEBUG(fmt::format("Detectado corchete [ en tipo: '{}'", inferredType));

                        std::string base = GetBaseType(inferredType);
                        if (base == "array" || base == "dictionary" || base == "grid")
                        {
                            inferredType = ExtractInnerType(inferredType);
                        }
                        else
                        {
                            std::string nextType = "";

                            asITypeInfo *t = engine ? engine->GetTypeInfoByDecl(inferredType.c_str()) : nullptr;
                            if (!t && engine)
                                t = engine->GetTypeInfoByName(base.c_str());

                            if (t)
                            {
                                for (asUINT m = 0; m < t->GetMethodCount(); m++)
                                {
                                    asIScriptFunction *func = t->GetMethodByIndex(m);
                                    if (func && std::string(func->GetName()) == "opIndex")
                                    {
                                        const char *decl = engine->GetTypeDeclaration(func->GetReturnTypeId(), true);
                                        if (decl)
                                            nextType = decl;
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
                        HARVEST_DEBUG(fmt::format("Tipo reducido matematicamente a: '{}'", inferredType));
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
                    continue;
                if (tok == "." || tok == "::" || tok == "new" || tok == "@")
                    continue;
                if (!isalpha(tok[0]) && tok[0] != '_')
                    continue;

                if (inferredType.empty())
                {
                    if (tok == "this")
                        inferredType = currentEnclosingClass;
                    else
                    {
                        for (const auto &v : locals)
                            if (v.name == tok)
                            {
                                inferredType = v.typeName;
                                break;
                            }
                        HARVEST_DEBUG(fmt::format("Raiz de la expresion '{}' resuelta como: '{}'", tok, inferredType));
                    }
                }
                else
                {
                    std::string baseType = GetBaseType(inferredType);
                    bool found = false;

                    HARVEST_DEBUG(fmt::format("Buscando miembro '{}' dentro de '{}'", tok, baseType));

                    for (const auto &c : customClasses)
                    {
                        if (c.name == baseType)
                        {
                            for (const auto &p : c.properties)
                                if (p.name == tok)
                                {
                                    inferredType = p.typeName;
                                    found = true;
                                    break;
                                }
                            if (!found)
                                for (const auto &m : c.methods)
                                    if (m.name == tok)
                                    {
                                        inferredType = m.typeName;
                                        found = true;
                                        break;
                                    }
                            break;
                        }
                    }

                    if (!found && engine)
                    {
                        asITypeInfo *t = engine->GetTypeInfoByName(baseType.c_str());
                        if (t)
                        {
                            for (asUINT p = 0; p < t->GetPropertyCount(); p++)
                            {
                                const char *pName = nullptr;
                                int pTypeId = 0;
                                t->GetProperty(p, &pName, &pTypeId);
                                if (pName && std::string(pName) == tok)
                                {
                                    const char *decl = engine->GetTypeDeclaration(pTypeId, true);
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
                                    asIScriptFunction *func = t->GetMethodByIndex(m);
                                    if (func && std::string(func->GetName()) == tok)
                                    {
                                        int rTypeId = func->GetReturnTypeId();
                                        const char *decl = engine->GetTypeDeclaration(rTypeId, true);
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
                    HARVEST_DEBUG(fmt::format("Resultado de buscar miembro '{}': '{}'", tok, inferredType));
                }
            }

            if (!inferredType.empty())
            {
                inferredType.erase(std::remove(inferredType.begin(), inferredType.end(), '@'), inferredType.end());
                inferredType.erase(std::remove(inferredType.begin(), inferredType.end(), '&'), inferredType.end());
                HARVEST_DEBUG(fmt::format("Auto inferido final: '{}'", inferredType));
                return inferredType;
            }

            HARVEST_DEBUG("Fallo al inferir Auto. Retornando 'auto'");
            return "auto";
        }

        /**
         * @brief Interprets potential variable declarations inside local method scopes.
         * @param savedPos State restoration point for failed parse evaluations.
         * @param directlyInClass Flag indicating if the processing context is directly within a class block scope.
         * @return True if a local variable was cleanly mapped.
         */
        bool ProcessLocalDeclaration(size_t savedPos, bool directlyInClass = false)
        {
            stream.SetPos(savedPos);
            std::string parsedType = ParseDataType(engine, stream, knownClassNames);
            if (parsedType.empty())
            {
                stream.SetPos(savedPos);
                return false;
            }

            asETokenClass tc = stream.Peek(token);

            if (tc != asTC_IDENTIFIER)
            {
                stream.SetPos(savedPos);
                return false;
            }

            std::string varName = std::string(token);
            stream.Advance(token);
            tc = stream.Peek(token);

            if (tc == asTC_KEYWORD && (token == ";" || token == "=" || token == "," || token == ")" || token == "("))
            {
                bool isLocal = false;
                int effectiveDepth = currentDepth;

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
                        HARVEST_DEBUG(fmt::format("NUEVA VARIABLE LOCAL: '{}' de tipo '{}' a profundidad {}", varName, parsedType, effectiveDepth));
                    }

                    ParseAssignments(parsedType, effectiveDepth, varName);
                    return true;
                }
            }

            stream.SetPos(savedPos);
            return false;
        }

        /**
         * @brief Captures multi-variable chains and initializations to deduce automated typing.
         * @param parsedType The designated variable type or 'auto'.
         * @param effectiveDepth The current block scope depth index.
         * @param currentVarName The initial variable assigned in the line.
         */
        void ParseAssignments(std::string parsedType, int effectiveDepth, std::string currentVarName)
        {
            bool isAssigning = false;
            std::vector<std::string> expressionTokens;

            while (true)
            {
                asETokenClass lookTc = stream.Peek(token);

                if (lookTc == asTC_UNKNOWN || (lookTc == asTC_KEYWORD && (token == ";" || token == ")")))
                {
                    if (isAssigning && parsedType == "auto" && !expressionTokens.empty())
                    {
                        std::string inferred = InferAutoType(expressionTokens);
                        if (inferred != "auto")
                        {
                            parsedType = inferred;
                            auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable &v)
                                                   { return v.name == currentVarName; });
                            if (it != locals.end())
                                it->typeName = parsedType;
                        }
                    }

                    stream.Advance(token);
                    if (token == ")")
                    {
                        if (parenDepth > 0)
                            parenDepth--;
                        lastTokenStr = ")";
                    }
                    else
                        lastTokenStr = ";";
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
                        std::string inferred = InferAutoType(expressionTokens);
                        if (inferred != "auto")
                        {
                            parsedType = inferred;
                            auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable &v)
                                                   { return v.name == currentVarName; });
                            if (it != locals.end())
                                it->typeName = parsedType;
                        }
                    }

                    stream.Advance(token);
                    lastTokenStr = ",";
                    isAssigning = false;
                    expressionTokens.clear();

                    stream.Peek(token);
                    if (IsValidDataType(engine, stream, token, knownClassNames))
                        break;

                    if (stream.Peek(token) == asTC_IDENTIFIER)
                    {
                        std::string nextVar = std::string(token);
                        stream.Advance(token);
                        lastTokenStr = nextVar;
                        currentVarName = nextVar;

                        auto it = std::find_if(locals.begin(), locals.end(), [&](const LocalVariable &v)
                                               { return v.name == nextVar; });
                        if (it == locals.end())
                            locals.push_back({nextVar, parsedType, effectiveDepth});
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
                        parenDepth++;
                    else if (token == ")")
                    {
                        if (parenDepth > 0)
                            parenDepth--;
                    }
                    lastTokenStr = std::string(token);
                }
            }
        }
    };

    /**
     * @brief Operates a contextual pass backwards from the cursor to isolate in-scope block variables.
     * @param engine Pointer to the active AngelScript engine.
     * @param code The source code string view.
     * @param cursorAbsolutePos Absolute index determining the cut-off bounds for scope mapping.
     * @param outEnclosingClass String reference populated with the immediate wrapping class frame, if any.
     * @param customClasses Extracted map of known user structures.
     * @param globalVars Extracted map of known global parameters.
     * @param globalFuncs Extracted map of known global routines.
     * @return A vector of active local variable bindings available to the cursor.
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
     * @param engine Pointer to the active AngelScript engine.
     * @param code The source code string view.
     * @return A vector defining all global functions located.
     */
    std::vector<GlobalFunction> ScanGlobalFunctions(asIScriptEngine *engine, std::string_view code)
    {
        std::vector<GlobalFunction> funcs;
        std::vector<ScriptClass> customClasses = ScanCustomClasses(engine, code);
        std::vector<std::string> localClasses;
        for (const auto &c : customClasses)
            localClasses.push_back(c.name);

        TokenStream stream(engine, code);
        int currentDepth = 0;
        std::string_view token;
        std::string lastTokenStr = "";

        while (stream.Peek(token) != asTC_UNKNOWN)
        {
            size_t savedPos = stream.GetPos();
            asETokenClass tc = stream.Peek(token);

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

            bool validDeclFound = false;

            if (lastTokenStr != "." && lastTokenStr != "->" && lastTokenStr != "::")
            {
                size_t beforeTypePos = stream.GetPos();
                std::string parsedType = ParseDataType(engine, stream, localClasses);

                if (!parsedType.empty() && stream.Peek(token) == asTC_IDENTIFIER)
                {
                    std::string funcName = std::string(token);
                    stream.Advance(token);

                    if (stream.Peek(token) == asTC_KEYWORD && token == "(")
                    {
                        validDeclFound = true;
                        if (currentDepth == 0)
                        {
                            std::string decl = fmt::format("{} {}()", parsedType, funcName);
                            auto it = std::find_if(funcs.begin(), funcs.end(), [&](const GlobalFunction &f)
                                                   { return f.name == funcName; });
                            if (it == funcs.end())
                                funcs.push_back({funcName, parsedType, decl});
                        }
                    }
                }
                if (!validDeclFound)
                    stream.SetPos(beforeTypePos);
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
     * @brief Parses an un-scoped script window for top-level mutable application data structures.
     * @param engine Pointer to the active AngelScript engine.
     * @param code The source code string view.
     * @return A vector of discovered global variables.
     */
    std::vector<GlobalVariable> ScanGlobalVariables(asIScriptEngine *engine, std::string_view code)
    {
        std::vector<GlobalVariable> vars;
        std::vector<ScriptClass> customClasses = ScanCustomClasses(engine, code);
        std::vector<std::string> localClasses;
        for (const auto &c : customClasses)
            localClasses.push_back(c.name);

        TokenStream stream(engine, code);
        int currentDepth = 0;
        std::string_view token;
        std::string lastTokenStr = "";

        while (stream.Peek(token) != asTC_UNKNOWN)
        {
            size_t savedPos = stream.GetPos();
            asETokenClass tc = stream.Peek(token);

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

            bool validDeclFound = false;

            if (lastTokenStr != "." && lastTokenStr != "->" && lastTokenStr != "::")
            {
                size_t beforeTypePos = stream.GetPos();
                std::string parsedType = ParseDataType(engine, stream, localClasses);

                if (!parsedType.empty() && stream.Peek(token) == asTC_IDENTIFIER)
                {
                    std::string varName = std::string(token);
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
                                vars.push_back({varName, parsedType});

                            while (true)
                            {
                                asETokenClass lookTc = stream.Peek(token);
                                if (lookTc == asTC_UNKNOWN || (lookTc == asTC_KEYWORD && token == ";"))
                                    break;

                                if (lookTc == asTC_KEYWORD && token == ",")
                                {
                                    stream.Advance(token);
                                    lastTokenStr = ",";
                                    if (stream.Peek(token) == asTC_IDENTIFIER)
                                    {
                                        std::string nextVar = std::string(token);
                                        stream.Advance(token);
                                        lastTokenStr = nextVar;

                                        it = std::find_if(vars.begin(), vars.end(), [&](const GlobalVariable &v)
                                                          { return v.name == nextVar; });
                                        if (it == vars.end())
                                            vars.push_back({nextVar, parsedType});
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
                    stream.SetPos(beforeTypePos);
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