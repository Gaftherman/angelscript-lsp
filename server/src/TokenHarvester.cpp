/**
 * @file TokenHarvester.cpp
 * @brief Implements highly-optimized, fault-tolerant grammar scanning for AngelScript LSP.
 */

#include "TokenHarvester.h"
#include "SafeCtype.h"
#include <sstream>
#include <algorithm>
#include <array>
#include <ranges>
#include <map>
#include <unordered_map>
#include <fmt/core.h>
#include <fstream>
#include <iostream>

namespace TokenHarvester
{
    namespace
    {
        constexpr std::array<std::string_view, 5> TYPE_DECLARATIONS = {
            "class", "interface", "namespace", "enum", "mixin"};

        constexpr std::array<std::string_view, 4> ACCESS_MODIFIERS = {
            "private", "protected", "public", "shared"};

        constexpr std::array<std::string_view, 6> GLOBAL_MODIFIERS = {
            "private", "protected", "public", "shared", "external", "mixin"};

        constexpr std::array<std::string_view, 3> PROPERTY_SEPARATORS = {
            ";", "=", ","};

        constexpr std::array<std::string_view, 19> BUILTIN_TYPES = {
            "void", "int", "int8", "int16", "int32", "int64",
            "uint", "uint8", "uint16", "uint32", "uint64",
            "float", "double", "bool", "auto", "string",
            "array", "dictionary", "grid"};

        constexpr std::array<std::string_view, 4> METHOD_MODIFIERS = {
            "const", "override", "final", "property"};

        constexpr std::array<std::string_view, 9> CONTROL_KEYWORDS = {
            "if", "for", "foreach", "while", "do", "switch", "try", "catch", "case"};

        inline void HARVEST_DEBUG(std::string_view msg)
        {
            std::cerr << "[Harvester] " << msg << std::endl;
        }
    }

    static inline bool IsTypeDeclaration(std::string_view token) noexcept
    {
        return std::ranges::any_of(TYPE_DECLARATIONS, [token](std::string_view t)
                                   { return t == token; });
    }

    static inline bool IsAccessModifier(std::string_view token) noexcept
    {
        return std::ranges::any_of(ACCESS_MODIFIERS, [token](std::string_view t)
                                   { return t == token; });
    }

    static inline bool IsGlobalModifier(std::string_view token) noexcept
    {
        return std::ranges::any_of(GLOBAL_MODIFIERS, [token](std::string_view t)
                                   { return t == token; });
    }

    static inline bool IsPropertySeparator(std::string_view token) noexcept
    {
        return std::ranges::any_of(PROPERTY_SEPARATORS, [token](std::string_view t)
                                   { return t == token; });
    }

    static inline bool IsBuiltInType(std::string_view token) noexcept
    {
        return std::ranges::any_of(BUILTIN_TYPES, [token](std::string_view t)
                                   { return t == token; });
    }

    static inline bool IsMethodModifier(std::string_view token) noexcept
    {
        return std::ranges::any_of(METHOD_MODIFIERS, [token](std::string_view t)
                                   { return t == token; });
    }

    static inline bool IsControlKeyword(std::string_view token) noexcept
    {
        return std::ranges::any_of(CONTROL_KEYWORDS, [token](std::string_view t)
                                   { return t == token; });
    }

    std::string_view GetBaseType(std::string_view type) noexcept
    {
        while (!type.empty() && (type.front() == ' ' || type.front() == '\t'))
        {
            type.remove_prefix(1);
        }

        if (type.starts_with("const "))
        {
            type.remove_prefix(6);
            while (!type.empty() && (type.front() == ' ' || type.front() == '\t'))
            {
                type.remove_prefix(1);
            }
        }

        if (size_t templatePos = type.find('<'); templatePos != std::string_view::npos)
        {
            type = type.substr(0, templatePos);
        }

        while (!type.empty())
        {
            char back = type.back();
            if (back == ' ' || back == '\t' || back == '\r' || back == '\n')
            {
                type.remove_suffix(1);
                continue;
            }
            if (type.ends_with("inout"))
            {
                type.remove_suffix(5);
                continue;
            }
            if (type.ends_with("out"))
            {
                type.remove_suffix(3);
                continue;
            }
            if (type.ends_with("in"))
            {
                if (type.length() == 2 || type[type.length() - 3] == ' ' || type[type.length() - 3] == '&')
                {
                    type.remove_suffix(2);
                    continue;
                }
            }
            if (back == '&' || back == '@')
            {
                type.remove_suffix(1);
                continue;
            }
            if (type.ends_with("const"))
            {
                type.remove_suffix(5);
                continue;
            }
            break;
        }

        if (type.find("[]") != std::string_view::npos)
        {
            return "array";
        }

        return type;
    }

    std::string_view ExtractInnerType(std::string_view type) noexcept
    {
        size_t start = type.find('<');
        size_t end = type.rfind('>');
        if (start != std::string_view::npos && end != std::string_view::npos && end > start)
        {
            return type.substr(start + 1, end - start - 1);
        }

        if (size_t bracket = type.rfind("[]"); bracket != std::string_view::npos)
        {
            return type.substr(0, bracket);
        }

        return type;
    }

    std::string_view GetInstantiatedType(std::string_view type) noexcept
    {
        while (!type.empty() && type.front() == ' ')
        {
            type.remove_prefix(1);
        }

        if (type.starts_with("const "))
        {
            type.remove_prefix(6);
            while (!type.empty() && type.front() == ' ')
            {
                type.remove_prefix(1);
            }
        }

        while (!type.empty())
        {
            char back = type.back();
            if (back == '@' || back == '&' || back == ' ')
            {
                type.remove_suffix(1);
            }
            else if (type.ends_with("const"))
            {
                type.remove_suffix(5);
            }
            else
            {
                break;
            }
        }

        return type;
    }

    class TokenStream
    {
    private:
        asIScriptEngine *engine;
        std::string_view code;
        size_t pos;

    public:
        TokenStream(asIScriptEngine *e, std::string_view c, size_t start = 0)
            : engine(e), code(c), pos(start) {}

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

        bool IsEOF()
        {
            std::string_view dummy;
            asETokenClass tc = Peek(dummy);
            return (tc == asTC_UNKNOWN && dummy.empty());
        }

        size_t GetPos() const { return pos; }
        void SetPos(size_t p) { pos = p; }
    };

    static bool IsValidDataType(asIScriptEngine *engine, TokenStream &stream, std::string_view token, const std::vector<std::string> &localClasses)
    {
        if (token == "const" || IsBuiltInType(token))
            return true;

        std::string tokenStr(token);
        if (engine && engine->GetTypeInfoByName(tokenStr.c_str()) != nullptr)
            return true;
        if (std::ranges::find(localClasses, tokenStr) != localClasses.end())
            return true;

        size_t savedPos = stream.GetPos();
        std::string_view t1, t2;
        asETokenClass tc1 = stream.Peek(t1);

        if (tc1 == asTC_KEYWORD && t1 == "::")
        {
            stream.SetPos(savedPos);
            return true;
        }
        if (tc1 == asTC_IDENTIFIER)
        {
            stream.SetPos(savedPos);
            return true;
        }
        if (tc1 == asTC_KEYWORD && (t1 == "@" || t1 == "&"))
        {
            stream.Advance(t1);
            asETokenClass tc2 = stream.Peek(t2);
            if (tc2 == asTC_KEYWORD && (t2 == "in" || t2 == "out" || t2 == "inout"))
            {
                stream.Advance(t2);
                tc2 = stream.Peek(t2);
            }
            if (tc2 == asTC_IDENTIFIER)
            {
                stream.SetPos(savedPos);
                return true;
            }
        }

        stream.SetPos(savedPos);
        return false;
    }

    static std::string ParseDataType(asIScriptEngine *engine, TokenStream &stream, const std::vector<std::string> &knownClassNames)
    {
        std::string typeName;
        std::string_view token;
        asETokenClass tc = stream.Peek(token);

        if (tc == asTC_KEYWORD && token == "const")
        {
            typeName += "const ";
            stream.Advance(token);
        }

        tc = stream.Peek(token);
        if (tc == asTC_KEYWORD && token == "::")
        {
            typeName += "::";
            stream.Advance(token);
        }

        tc = stream.Peek(token);
        bool isPrimitive = (tc == asTC_KEYWORD && IsBuiltInType(token));

        if (tc == asTC_IDENTIFIER || isPrimitive)
        {
            typeName += std::string(token);
            stream.Advance(token);

            while (!stream.IsEOF())
            {
                tc = stream.Peek(token);
                if (tc == asTC_KEYWORD && token == "::")
                {
                    typeName += "::";
                    stream.Advance(token);
                    tc = stream.Peek(token);
                    if (tc == asTC_IDENTIFIER)
                    {
                        typeName += std::string(token);
                        stream.Advance(token);
                    }
                }
                else
                    break;
            }
        }
        else
            return "";

        tc = stream.Peek(token);
        if (tc == asTC_KEYWORD && token == "<")
        {
            typeName += "<";
            stream.Advance(token);
            int angleDepth = 1;
            while (angleDepth > 0 && !stream.IsEOF())
            {
                stream.Advance(token);
                typeName += std::string(token);
                if (token == "<")
                    angleDepth++;
                else if (token == ">")
                    angleDepth--;
            }
        }

        while (!stream.IsEOF())
        {
            size_t currentPos = stream.GetPos();
            std::string_view t1, t2;
            if (stream.Peek(t1) == asTC_KEYWORD && t1 == "[")
            {
                stream.Advance(t1);
                if (stream.Peek(t2) == asTC_KEYWORD && t2 == "]")
                {
                    stream.Advance(t2);
                    typeName = "array<" + typeName + ">";
                    continue;
                }
            }
            stream.SetPos(currentPos);
            break;
        }

        while (!stream.IsEOF())
        {
            tc = stream.Peek(token);
            if (tc == asTC_KEYWORD && (token == "@" || token == "&"))
            {
                typeName += std::string(token);
                stream.Advance(token);
            }
            else
                break;
        }

        tc = stream.Peek(token);
        if (tc == asTC_KEYWORD && (token == "in" || token == "out" || token == "inout"))
        {
            typeName += " " + std::string(token);
            stream.Advance(token);
        }

        return typeName;
    }

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

    CompletionContext GetCompletionContext(asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos)
    {
        CompletionContext ctx;
        TokenStream stream(engine, code);
        std::string_view token;
        std::vector<std::string> tokens;
        std::vector<std::string> chain;

        ctx.isMemberAccess = false;
        ctx.partialMember = "";
        ctx.lastSeparator = "";

        while (stream.GetPos() < cursorAbsolutePos && !stream.IsEOF())
        {
            stream.Advance(token);
            tokens.push_back(std::string(token));
        }

        if (tokens.empty())
            return ctx;

        int i = static_cast<int>(tokens.size()) - 1;

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
            std::string lastTok = tokens[i];
            if (!lastTok.empty() && (SAFE_IS_ALPHA(lastTok[0]) || lastTok[0] == '_'))
            {
                ctx.partialMember = lastTok;
                i--;
            }
            if (i >= 0 && tokens[i] == ":")
            {
                ctx.lastSeparator = ":";
                if (i >= 1)
                    ctx.objectChain.push_back(tokens[i - 1]);
            }
        }

        if (ctx.isMemberAccess)
        {
            while (i >= 0)
            {
                std::string fullIdent;
                while (i >= 0 && (tokens[i] == "]" || tokens[i] == ")"))
                {
                    std::string closingToken = tokens[i];
                    std::string openingToken = (closingToken == "]") ? "[" : "(";
                    int depth = 0;
                    std::string segment;
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
                    std::string tok = tokens[i];
                    if (!tok.empty() && (SAFE_IS_ALPHA(tok[0]) || tok[0] == '_' || tok == "this" || tok == "super"))
                    {
                        fullIdent = tok + fullIdent;
                        chain.insert(chain.begin(), fullIdent);
                        i--;
                    }
                    else
                        break;
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
        ASTScanner(asIScriptEngine *e, std::string_view c)
            : engine(e), code(c), stream(e, c), currentDepth(0) {}

    protected:
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

        void SkipBlock()
        {
            int methodDepth = 1;
            while (methodDepth > 0 && !stream.IsEOF())
            {
                stream.Advance(token);
                if (token == "{")
                    methodDepth++;
                else if (token == "}")
                    methodDepth--;
            }
        }

        void CollectKnownTypes()
        {
            TokenStream quickStream(engine, code);
            std::vector<std::string> quickScope;
            std::string qPendingName;
            std::string qPendingType;
            std::string fScope;
            int qDepth = 0;

            while (!quickStream.IsEOF())
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
                        fScope.clear();
                        for (const auto &s : quickScope)
                        {
                            if (!fScope.empty())
                                fScope += "::";
                            fScope += s;
                        }
                        knownClassNames.push_back(fScope);
                        qPendingName.clear();
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

    class CustomClassScanner : public ASTScanner
    {
    private:
        std::map<std::string, ScriptClass> classMap;
        std::string currentAccess;

    public:
        CustomClassScanner(asIScriptEngine *e, std::string_view c) : ASTScanner(e, c), currentAccess("public")
        {
            CollectKnownTypes();
        }

        std::vector<ScriptClass> Scan()
        {
            std::vector<std::string> bases;
            std::string fullScope;
            std::vector<ScriptClass> results;

            while (!stream.IsEOF())
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
                        bases.clear();

                        if (stream.Peek(token) == asTC_KEYWORD && token == ":")
                        {
                            stream.Advance(token);
                            while (!stream.IsEOF())
                            {
                                if (stream.Peek(token) == asTC_KEYWORD && token == "{")
                                    break;
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

                            pendingScopeName.clear();
                            pendingScopeType.clear();
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
                        pendingScopeName.clear();
                        pendingScopeType.clear();
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
                    validDeclFound = ProcessMember(savedPos);

                if (validDeclFound)
                    currentAccess = "public";
                else if (stream.GetPos() == savedPos)
                    stream.Advance(token);
            }

            results.reserve(classMap.size());
            for (auto &pair : classMap)
            {
                results.push_back(std::move(pair.second));
            }
            return results;
        }

    private:
        bool ProcessEnum(asETokenClass tc)
        {
            if (scopeStack.back().type != "enum")
                return false;
            if (tc == asTC_IDENTIFIER)
            {
                std::string propName(token);
                classMap[GetFullScope()].properties[propName] = ClassProperty{propName, "int", "public"};
            }
            return true;
        }

        bool ProcessConstructorOrDestructor(asETokenClass tc)
        {
            if (scopeStack.empty())
                return false;
            ScopeCtx currentScope = scopeStack.back();
            std::string fullScope = GetFullScope();
            std::string_view nextToken;

            if (tc == asTC_IDENTIFIER && token == currentScope.name)
            {
                if (stream.Peek(nextToken) == asTC_KEYWORD && nextToken == "(")
                {
                    int parenDepth = 0;
                    do
                    {
                        stream.Advance(token);
                        if (token == "(")
                            parenDepth++;
                        else if (token == ")")
                            parenDepth--;
                    } while (parenDepth > 0 && !stream.IsEOF());

                    classMap[fullScope].methods[currentScope.name].push_back({currentScope.name, "void", fullScope + "()", currentAccess, true});

                    while (!stream.IsEOF())
                    {
                        if (stream.Peek(nextToken) == asTC_KEYWORD && (nextToken == "{" || nextToken == ";"))
                            break;
                        stream.Advance(token);
                    }

                    if (stream.Peek(nextToken) == asTC_KEYWORD && nextToken == "{")
                    {
                        stream.Advance(token);
                        SkipBlock();
                    }
                    else if (stream.Peek(nextToken) == asTC_KEYWORD && nextToken == ";")
                    {
                        stream.Advance(token);
                    }

                    currentAccess = "public";
                    return true;
                }
            }
            else if (tc == asTC_KEYWORD && token == "~")
            {
                if (stream.Peek(nextToken) == asTC_IDENTIFIER && nextToken == currentScope.name)
                {
                    stream.Advance(token);
                    if (stream.Peek(nextToken) == asTC_KEYWORD && nextToken == "(")
                    {
                        int parenDepth = 0;
                        do
                        {
                            stream.Advance(token);
                            if (token == "(")
                                parenDepth++;
                            else if (token == ")")
                                parenDepth--;
                        } while (parenDepth > 0 && !stream.IsEOF());

                        std::string destName = "~" + currentScope.name;
                        classMap[fullScope].methods[destName].push_back({destName, "void", "~" + fullScope + "()", currentAccess, true});

                        while (!stream.IsEOF())
                        {
                            if (stream.Peek(nextToken) == asTC_KEYWORD && (nextToken == "{" || nextToken == ";"))
                                break;
                            stream.Advance(token);
                        }

                        if (stream.Peek(nextToken) == asTC_KEYWORD && nextToken == "{")
                        {
                            stream.Advance(token);
                            SkipBlock();
                        }
                        else if (stream.Peek(nextToken) == asTC_KEYWORD && nextToken == ";")
                        {
                            stream.Advance(token);
                        }

                        currentAccess = "public";
                        return true;
                    }
                }
            }
            return false;
        }

        bool ProcessMember(size_t savedPos)
        {
            stream.SetPos(savedPos);
            std::string parsedType = ParseDataType(engine, stream, knownClassNames);

            if (parsedType.empty() || stream.Peek(token) != asTC_IDENTIFIER)
            {
                stream.SetPos(savedPos);
                return false;
            }

            std::string memberName(token);
            stream.Advance(token);
            asETokenClass lookTc = stream.Peek(token);

            if (lookTc == asTC_KEYWORD && token == "(")
            {
                ParseMethod(parsedType, memberName);
                currentAccess = "public";
                return true;
            }

            if (lookTc == asTC_KEYWORD && IsPropertySeparator(token))
            {
                ParseProperty(parsedType, memberName);
                currentAccess = "public";
                return true;
            }

            if (lookTc == asTC_KEYWORD && token == "{")
            {
                std::string fullScope = GetFullScope();
                classMap[fullScope].properties[memberName] = ClassProperty{memberName, parsedType, currentAccess};
                stream.Advance(token);
                SkipBlock();
                currentAccess = "public";
                return true;
            }

            if (lookTc == asTC_KEYWORD && (token == ";" || token == "="))
            {
                std::string fullScope = GetFullScope();
                classMap[fullScope].properties[memberName] = ClassProperty{memberName, parsedType, currentAccess};

                if (token == "=")
                {
                    while (!stream.IsEOF())
                    {
                        stream.Advance(token);
                        if (token == ";")
                            break;
                    }
                }
                else
                    stream.Advance(token);

                currentAccess = "public";
                return true;
            }

            stream.SetPos(savedPos);
            return false;
        }

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
            } while (parenDepth > 0 && !stream.IsEOF());

            size_t sigEnd = stream.GetPos();
            std::string exactParams(code.data() + sigStart, sigEnd - sigStart);
            std::string signature = parsedType + " " + fullScope + "::" + memberName + exactParams;

            classMap[fullScope].methods[memberName].push_back({memberName, parsedType, signature, currentAccess, false});

            while (!stream.IsEOF())
            {
                if (stream.Peek(token) == asTC_KEYWORD && (token == "{" || token == ";"))
                    break;
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

        void ParseProperty(const std::string &parsedType, const std::string &memberName)
        {
            std::string fullScope = GetFullScope();
            classMap[fullScope].properties[memberName] = ClassProperty{memberName, parsedType, currentAccess};

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
                        std::string nextVar(token);
                        stream.Advance(token);
                        classMap[fullScope].properties[nextVar] = ClassProperty{nextVar, parsedType, currentAccess};
                        continue;
                    }
                }
                else
                    stream.Advance(token);
            }
        }
    };

    std::vector<ScriptClass> ScanCustomClasses(asIScriptEngine *engine, std::string_view code)
    {
        CustomClassScanner scanner(engine, code);
        return scanner.Scan();
    }

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
        LocalVariableScanner(asIScriptEngine *e, std::string_view c, size_t pos,
                             const std::vector<ScriptClass> &cClasses,
                             const std::vector<GlobalVariable> &gVars,
                             const std::vector<GlobalFunction> &gFuncs)
            : ASTScanner(e, c), lastControlKeyword(""), parenDepth(0), cursorAbsolutePos(pos),
              customClasses(cClasses), globalVars(gVars), globalFuncs(gFuncs)
        {
            CollectKnownTypes();
        }

        std::string GetVariableNature(int pDepth, int effDepth) const
        {
            if (pDepth > 0)
            {
                if (lastControlKeyword == "for")
                    return "for loop control variable";
                if (blockStack.empty())
                    return "function parameter";
            }

            for (auto it = blockStack.rbegin(); it != blockStack.rend(); ++it)
            {
                if (it->type == "for" || it->type == "while" || it->type == "foreach" || it->type == "do")
                {
                    return fmt::format("local variable inside a {} loop block", it->type);
                }
                if (it->type == "if")
                    return "local variable inside an if block";
                if (it->type == "switch" || it->type == "case")
                    return "local variable inside a switch case";
                if (it->type == "try" || it->type == "catch")
                    return "local variable inside a try/catch block";
                if (it->type == "function")
                    return "local variable";
            }
            return "local variable";
        }

        std::vector<LocalVariable> Scan(std::string &outEnclosingClass)
        {
            std::string foreachType;
            std::string loopVar;
            std::vector<std::string> containerExpr;
            std::string blockType;

            while (stream.GetPos() < cursorAbsolutePos && !stream.IsEOF())
            {
                size_t savedPos = stream.GetPos();
                asETokenClass tc = stream.Peek(token);

                if ((tc == asTC_KEYWORD || tc == asTC_IDENTIFIER) && token == "function")
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
                                std::string paramName(token);
                                stream.Advance(token);
                                locals.push_back({paramName, paramType, currentDepth + 1});
                            }
                            else
                                stream.Advance(token);

                            if (stream.Peek(token) == asTC_KEYWORD && token == ",")
                                stream.Advance(token);
                        }
                        if (stream.Peek(token) == asTC_KEYWORD && token == ")")
                            stream.Advance(token);
                    }
                    lastTokenStr = ")";
                    continue;
                }

                if (tc == asTC_KEYWORD)
                {
                    if (IsControlKeyword(token))
                        lastControlKeyword = std::string(token);
                    else if (token == ";" && parenDepth == 0)
                        lastControlKeyword.clear();
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
                        lastControlKeyword.clear();
                    }
                    else
                    {
                        bool insideClassDecl = (!scopeStack.empty() && scopeStack.back().type == "class" && (currentDepth - 1) == scopeStack.back().depth);
                        if (scopeStack.empty() || insideClassDecl)
                            blockType = "function";
                    }

                    blockStack.push_back({blockType, currentDepth});

                    if (!pendingScopeName.empty())
                    {
                        scopeStack.push_back({pendingScopeName, pendingScopeType, currentDepth});
                        pendingScopeName.clear();
                        pendingScopeType.clear();
                    }

                    stream.Advance(token);
                    lastTokenStr = "{";
                    continue;
                }

                if (tc == asTC_KEYWORD && token == "}")
                {
                    if (!blockStack.empty() && blockStack.back().depth == currentDepth)
                        blockStack.pop_back();
                    if (!scopeStack.empty() && scopeStack.back().depth == currentDepth)
                        scopeStack.pop_back();

                    currentDepth--;
                    int captureDepth = currentDepth;

                    std::erase_if(locals, [captureDepth](const LocalVariable &v)
                                  { return v.declarationDepth > captureDepth; });

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
                                std::string containerType = InferAutoType(containerExpr);
                                std::string_view inner = ExtractInnerType(containerType);

                                if (!inner.empty() && inner != containerType)
                                {
                                    foreachType = (foreachType.back() == '@' && inner.back() != '@') ? std::string(inner) + "@" : std::string(inner);
                                }
                            }
                            locals.push_back({loopVar, foreachType, currentDepth + 1});
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

                bool directlyInClass = (!scopeStack.empty() && scopeStack.back().type == "class" && currentDepth == scopeStack.back().depth);
                bool validDeclFound = false;

                if (lastTokenStr != "." && lastTokenStr != "::")
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
        std::string InferAutoType(const std::vector<std::string> &tokens)
        {
            if (tokens.empty())
                return "auto";
            if (IsBuiltInType(tokens[0]))
                return std::string(tokens[0]);
            if (tokens[0].front() == '"' || tokens[0].front() == '\'')
                return "string";
            if (tokens[0] == "true" || tokens[0] == "false")
                return "bool";
            if (isdigit(static_cast<unsigned char>(tokens[0][0])))
            {
                if (tokens[0].find_first_of(".f") != std::string::npos)
                    return "float";
                return "int";
            }

            std::string inferredType;
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
                        std::string_view base = GetBaseType(inferredType);
                        if (base == "array" || base == "dictionary" || base == "grid")
                        {
                            inferredType = ExtractInnerType(inferredType);
                        }
                        else
                        {
                            std::string nextType;
                            asITypeInfo *t = engine ? engine->GetTypeInfoByDecl(inferredType.c_str()) : nullptr;
                            if (!t && engine)
                                t = engine->GetTypeInfoByName(std::string(base).c_str());
                            if (t)
                            {
                                for (asUINT m = 0; m < t->GetMethodCount(); m++)
                                {
                                    asIScriptFunction *func = t->GetMethodByIndex(m);
                                    if (func && std::string_view(func->GetName()) == "opIndex")
                                    {
                                        if (const char *decl = engine->GetTypeDeclaration(func->GetReturnTypeId(), true))
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
                                        if (auto it = c.methods.find("opIndex"); it != c.methods.end() && !it->second.empty())
                                        {
                                            nextType = it->second.front().typeName;
                                        }
                                        break;
                                    }
                                }
                            }
                            if (!nextType.empty())
                                inferredType = GetInstantiatedType(nextType);
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
                    continue;
                if (tok == "." || tok == "::" || tok == "@")
                    continue;
                if (!isalpha(static_cast<unsigned char>(tok[0])) && tok[0] != '_')
                    continue;

                if (inferredType.empty())
                {
                    if (tok == "this")
                        inferredType = currentEnclosingClass;
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
                    std::string_view baseType = GetBaseType(inferredType);
                    bool found = false;
                    for (const auto &c : customClasses)
                    {
                        if (c.name == baseType)
                        {
                            if (auto propIt = c.properties.find(tok); propIt != c.properties.end())
                            {
                                inferredType = propIt->second.typeName;
                                found = true;
                                break;
                            }
                            if (auto methodIt = c.methods.find(tok); methodIt != c.methods.end() && !methodIt->second.empty())
                            {
                                inferredType = methodIt->second.front().typeName;
                                found = true;
                                break;
                            }
                            break;
                        }
                    }

                    if (!found && engine)
                    {
                        if (asITypeInfo *t = engine->GetTypeInfoByName(std::string(baseType).c_str()))
                        {
                            for (asUINT p = 0; p < t->GetPropertyCount(); p++)
                            {
                                const char *pName = nullptr;
                                int pTypeId = 0;
                                t->GetProperty(p, &pName, &pTypeId);
                                if (pName && tok == pName)
                                {
                                    if (const char *decl = engine->GetTypeDeclaration(pTypeId, true))
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
                                    if (func && tok == func->GetName())
                                    {
                                        if (const char *decl = engine->GetTypeDeclaration(func->GetReturnTypeId(), true))
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
                std::erase(inferredType, '@');
                std::erase(inferredType, '&');
                return inferredType;
            }
            return "auto";
        }

        bool ProcessLocalDeclaration(size_t savedPos, bool directlyInClass)
        {
            stream.SetPos(savedPos);
            std::string parsedType = ParseDataType(engine, stream, knownClassNames);
            if (parsedType.empty())
            {
                stream.SetPos(savedPos);
                return false;
            }

            std::string_view baseType = GetBaseType(parsedType);
            if (!IsValidDataType(engine, stream, baseType, knownClassNames))
            {
                stream.SetPos(savedPos);
                return false;
            }

            if (stream.Peek(token) != asTC_IDENTIFIER)
            {
                stream.SetPos(savedPos);
                return false;
            }

            std::string varName(token);
            stream.Advance(token);
            asETokenClass tc = stream.Peek(token);

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
                    auto it = std::ranges::find_if(locals, [&](const LocalVariable &v)
                                                   { return v.name == varName; });
                    if (it == locals.end())
                        locals.push_back({varName, parsedType, effectiveDepth});

                    ParseAssignments(parsedType, effectiveDepth, varName);
                    return true;
                }
            }

            stream.SetPos(savedPos);
            return false;
        }

        void ParseAssignments(std::string parsedType, int effectiveDepth, std::string currentVarName)
        {
            std::vector<std::string> expressionTokens;
            std::string nextVar;
            bool isAssigning = false;

            while (!stream.IsEOF())
            {
                asETokenClass lookTc = stream.Peek(token);
                if ((lookTc == asTC_KEYWORD || lookTc == asTC_IDENTIFIER) && token == "function")
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
                                std::string paramName(token);
                                stream.Advance(token);
                                locals.push_back({paramName, paramType, currentDepth + 1});
                            }
                            else
                                stream.Advance(token);
                            if (stream.Peek(token) == asTC_KEYWORD && token == ",")
                                stream.Advance(token);
                        }
                        if (stream.Peek(token) == asTC_KEYWORD && token == ")")
                            stream.Advance(token);
                    }
                    if (stream.Peek(token) == asTC_KEYWORD && token == "{")
                        break;
                    continue;
                }

                if (lookTc == asTC_UNKNOWN || (lookTc == asTC_KEYWORD && (token == ";" || token == ")")))
                {
                    if (isAssigning && parsedType.find("auto") != std::string::npos && !expressionTokens.empty())
                    {
                        std::string inferred = InferAutoType(expressionTokens);
                        if (inferred != "auto")
                        {
                            parsedType = inferred;
                            auto it = std::ranges::find_if(locals, [&](const LocalVariable &v)
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
                    if (parenDepth > 0)
                        break;

                    if (isAssigning && parsedType.find("auto") != std::string::npos && !expressionTokens.empty())
                    {
                        std::string inferred = InferAutoType(expressionTokens);
                        if (inferred != "auto")
                        {
                            parsedType = inferred;
                            auto it = std::ranges::find_if(locals, [&](const LocalVariable &v)
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
                        nextVar = std::string(token);
                        stream.Advance(token);
                        lastTokenStr = nextVar;
                        currentVarName = nextVar;

                        auto it = std::ranges::find_if(locals, [&](const LocalVariable &v)
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
                        expressionTokens.push_back(std::string(token));
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

    std::vector<LocalVariable> ScanLocalVariables(
        asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos, std::string &outEnclosingClass,
        const std::vector<ScriptClass> &customClasses, const std::vector<GlobalVariable> &globalVars, const std::vector<GlobalFunction> &globalFuncs)
    {
        LocalVariableScanner scanner(engine, code, cursorAbsolutePos, customClasses, globalVars, globalFuncs);
        return scanner.Scan(outEnclosingClass);
    }

    std::vector<GlobalFunction> ScanGlobalFunctions(asIScriptEngine *engine, std::string_view code)
    {
        std::vector<GlobalFunction> funcs;
        std::vector<std::string> localClasses;
        TokenStream stream(engine, code);
        std::string lastTokenStr;
        std::string parsedType;
        std::string funcName;
        std::string_view token;
        int currentDepth = 0;

        std::vector<ScriptClass> customClasses = ScanCustomClasses(engine, code);
        localClasses.reserve(customClasses.size());
        for (const auto &c : customClasses)
            localClasses.push_back(c.name);

        while (!stream.IsEOF())
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
            if (lastTokenStr != "." && lastTokenStr != "::")
            {
                size_t beforeTypePos = stream.GetPos();
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
                            std::string decl = fmt::format("{} {}()", parsedType, funcName);
                            auto it = std::ranges::find_if(funcs, [&](const GlobalFunction &f)
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

    std::vector<GlobalVariable> ScanGlobalVariables(asIScriptEngine *engine, std::string_view code)
    {
        std::vector<GlobalVariable> vars;
        std::vector<std::string> localClasses;
        TokenStream stream(engine, code);
        std::string lastTokenStr;
        std::string parsedType;
        std::string varName;
        std::string nextVar;
        std::string_view token;
        int currentDepth = 0;

        std::vector<ScriptClass> customClasses = ScanCustomClasses(engine, code);
        localClasses.reserve(customClasses.size());
        for (const auto &c : customClasses)
            localClasses.push_back(c.name);

        while (!stream.IsEOF())
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
            if (lastTokenStr != "." && lastTokenStr != "::")
            {
                size_t beforeTypePos = stream.GetPos();
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
                            auto it = std::ranges::find_if(vars, [&](const GlobalVariable &v)
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
                                        nextVar = std::string(token);
                                        stream.Advance(token);
                                        lastTokenStr = nextVar;
                                        it = std::ranges::find_if(vars, [&](const GlobalVariable &v)
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
}