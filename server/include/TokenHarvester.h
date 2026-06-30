/**
 * @file TokenHarvester.h
 * @brief Fault-tolerant textual token scanning utilities for context parsing and definition tracking.
 * @author AngelScript LSP Team
 */

#ifndef TOKEN_HARVESTER_H
#define TOKEN_HARVESTER_H

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <map>
#include <functional>
#include <angelscript.h>

namespace TokenHarvester
{
    struct DeclarationLocation
    {
        std::string uri;
        int line = 0;
        int character = 0;
    };

    struct LocalVariable
    {
        std::string name;
        std::string typeName;
        int declarationDepth;
        DeclarationLocation location;
    };

    struct GlobalFunction
    {
        std::string name;
        std::string typeName;
        std::string declaration;
        DeclarationLocation location;
    };

    struct GlobalVariable
    {
        std::string name;
        std::string typeName;
        DeclarationLocation location;
    };

    struct ClassProperty
    {
        std::string name;
        std::string typeName;
        std::string access;
        DeclarationLocation location;
    };

    struct ClassMethod
    {
        std::string name;
        std::string typeName;
        std::string declaration;
        std::string access;
        bool isConstructor;
        DeclarationLocation location;
    };

    struct ScriptClass
    {
        std::string name;
        std::vector<std::string> baseTypes;
        std::unordered_map<std::string, ClassProperty> properties;
        std::unordered_map<std::string, std::vector<ClassMethod>> methods;
        DeclarationLocation location;
    };

    struct CompletionContext
    {
        bool isMemberAccess;
        std::vector<std::string> objectChain;
        std::string partialMember;
        std::string lastSeparator;
    };

    /**
     * @class TokenStream
     * @brief Fault-tolerant lexical token streaming wrapper for parsing AngelScript source buffers.
     */
    class TokenStream
    {
    private:
        asIScriptEngine *engine;
        std::string_view code;
        size_t pos;

    public:
        TokenStream(asIScriptEngine *e, std::string_view c, size_t start = 0);

        asETokenClass Peek(std::string_view &outToken);
        asETokenClass Advance(std::string_view &outToken);
        bool IsEOF();
        size_t GetPos() const { return pos; }
        void SetPos(size_t p) { pos = p; }
    };

    namespace Service
    {
        /**
         * @brief Standardized abstraction to process global symbols via callable visitors.
         */
        template <typename TAction>
        void HarvestGlobalTokens(asIScriptEngine *engine, std::string_view code, const std::vector<ScriptClass> &customClasses, TAction callback)
        {
            std::vector<std::string> localClasses;
            localClasses.reserve(customClasses.size());

            for (const auto &c : customClasses)
            {
                localClasses.push_back(c.name);
            }

            TokenStream stream(engine, code);
            std::string lastTokenStr;
            int currentDepth = 0;

            while (!stream.IsEOF())
            {
                size_t savedPos = stream.GetPos();
                std::string_view token;
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

                callback(stream, tc, token, lastTokenStr, currentDepth, savedPos, localClasses);
            }
        }
    }

    /**
     * @class ASTScanner
     * @brief Base class for syntax tree traversal operations providing scoping and validation blocks.
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
        ASTScanner(asIScriptEngine *e, std::string_view c);
        virtual ~ASTScanner() = default;

    protected:
        std::string GetFullScope() const;
        void SkipBlock();
        void CollectKnownTypes();
    };

    /**
     * @class CustomClassScanner
     * @brief Context framework specialized in harvesting user-defined script classes and properties.
     */
    class CustomClassScanner : public ASTScanner
    {
    private:
        std::map<std::string, ScriptClass> classMap;
        std::string currentAccess;

    public:
        CustomClassScanner(asIScriptEngine *e, std::string_view c);
        virtual ~CustomClassScanner() = default;

        std::vector<ScriptClass> Scan();

    private:
        bool ProcessEnum(asETokenClass tc);
        bool ProcessConstructorOrDestructor(asETokenClass tc);
        bool ProcessMember(size_t savedPos);
        void ParseMethod(const std::string &parsedType, const std::string &memberName);
        void ParseProperty(const std::string &parsedType, const std::string &memberName);
    };

    /**
     * @class LocalVariableScanner
     * @brief Introspects localized execution blocks to extract and map out current active scope variables.
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
        LocalVariableScanner(asIScriptEngine *e, std::string_view c, size_t pos,
                             const std::vector<ScriptClass> &cClasses,
                             const std::vector<GlobalVariable> &gVars,
                             const std::vector<GlobalFunction> &gFuncs);
        virtual ~LocalVariableScanner() = default;

        std::string GetVariableNature(int pDepth, int effDepth) const;
        std::vector<LocalVariable> Scan(std::string &outEnclosingClass);

    private:
        std::string InferAutoType(const std::vector<std::string> &tokens);
        bool ProcessLocalDeclaration(size_t savedPos, bool directlyInClass);
        void ParseAssignments(std::string parsedType, int effectiveDepth, std::string currentVarName);
    };

    std::string_view GetBaseType(std::string_view type) noexcept;
    std::string_view ExtractInnerType(std::string_view type) noexcept;
    std::string_view GetInstantiatedType(std::string_view type) noexcept;
    std::vector<ScriptClass> ScanCustomClasses(asIScriptEngine *engine, std::string_view code);
    CompletionContext GetCompletionContext(asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos);
    size_t GetAbsolutePosition(std::string_view text, int line, int character);
    std::vector<LocalVariable> ScanLocalVariables(asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos, std::string &outEnclosingClass, const std::vector<ScriptClass> &customClasses, const std::vector<GlobalVariable> &globalVars, const std::vector<GlobalFunction> &globalFuncs);
    std::vector<GlobalFunction> ScanGlobalFunctions(asIScriptEngine *engine, std::string_view code);
    std::vector<GlobalVariable> ScanGlobalVariables(asIScriptEngine *engine, std::string_view code);
}

#endif // TOKEN_HARVESTER_H