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
#include <angelscript.h>

namespace TokenHarvester
{
    /**
     * @struct DeclarationLocation
     * @brief Represents the exact coordinate positioning of a symbol declaration inside a specific file resource.
     */
    struct DeclarationLocation
    {
        /** @brief The absolute file resource URI string identifier. */
        std::string uri;

        /** @brief The zero-based row line index of the declaration. */
        int line = 0;

        /** @brief The zero-based column character offset index of the declaration. */
        int character = 0;
    };

    /**
     * @struct LocalVariable
     * @brief Context storage for variables alive within current functional execution scopes.
     */
    struct LocalVariable
    {
        /** @brief The name identifier of the local variable. */
        std::string name;

        /** @brief The data type identifier for the variable. */
        std::string typeName;

        /** @brief Track curly brace nesting depth at instantiation time. */
        int declarationDepth;

        /** @brief Concrete definition tracking coordinates for definition jumps. */
        DeclarationLocation location;
    };

    /**
     * @struct GlobalFunction
     * @brief Context storage for globally declared functions.
     */
    struct GlobalFunction
    {
        /** @brief The identifier name of the global function. */
        std::string name;

        /** @brief The return type identifier of the global function. */
        std::string typeName;

        /** @brief The full raw signature declaration statement of the function. */
        std::string declaration;

        /** @brief Concrete definition tracking coordinates for definition jumps. */
        DeclarationLocation location;
    };

    /**
     * @struct GlobalVariable
     * @brief Context storage for globally declared variables.
     */
    struct GlobalVariable
    {
        /** @brief The identifier name of the global variable. */
        std::string name;

        /** @brief The data type identifier of the global variable. */
        std::string typeName;

        /** @brief Concrete definition tracking coordinates for definition jumps. */
        DeclarationLocation location;
    };

    /**
     * @struct ClassProperty
     * @brief Context storage for class properties, including access specifiers.
     */
    struct ClassProperty
    {
        /** @brief The identifier name of the class property. */
        std::string name;

        /** @brief The data type identifier of the property. */
        std::string typeName;

        /** @brief The access visibility specifier (e.g., "public", "private", "protected"). */
        std::string access;

        /** @brief Concrete definition tracking coordinates for definition jumps. */
        DeclarationLocation location;
    };

    /**
     * @struct ClassMethod
     * @brief Context storage for class methods, tracking signatures and overloads.
     */
    struct ClassMethod
    {
        /** @brief The identifier name of the method. */
        std::string name;

        /** @brief The return type identifier of the class method. */
        std::string typeName;

        /** @brief The full signature statement declaration of the class method. */
        std::string declaration;

        /** @brief The access visibility specifier (e.g., "public", "private", "protected"). */
        std::string access;

        /** @brief Flag indicating if this method serves as an initialization constructor. */
        bool isConstructor;

        /** @brief Concrete definition tracking coordinates for definition jumps. */
        DeclarationLocation location;
    };

    /**
     * @struct ScriptClass
     * @brief Represents a user-defined class fully populated with O(1) associative lookup tables.
     */
    struct ScriptClass
    {
        /** @brief The identifier name of the user-defined script class. */
        std::string name;

        /** @brief List of parent class identifiers used for tree resolution. */
        std::vector<std::string> baseTypes;

        /** @brief Map of encapsulating property variables indexed for O(1) tracking lookups. */
        std::unordered_map<std::string, ClassProperty> properties;

        /** @brief Map of encapsulated methods and polymorph overloads indexed for O(1) tracking lookups. */
        std::unordered_map<std::string, std::vector<ClassMethod>> methods;

        /** @brief Concrete definition tracking coordinates for definition jumps. */
        DeclarationLocation location;
    };

    /**
     * @struct CompletionContext
     * @brief Natively resolved context for autocompletion triggering.
     */
    struct CompletionContext
    {
        /** @brief Flag indicating if the completion trigger is a member access. */
        bool isMemberAccess;

        /** @brief The sequence of accessed objects, supporting infinite chaining (e.g., A.B.C). */
        std::vector<std::string> objectChain;

        /** @brief The incomplete or partial member string currently being typed. */
        std::string partialMember;

        /** @brief The structural separator token context triggering completion. */
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