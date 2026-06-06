/**
 * @file TokenHarvester.h
 * @brief Fault-tolerant textual token scanning utilities for context parsing.
 * @author AngelScript LSP Team
 */

#ifndef TOKEN_HARVESTER_H
#define TOKEN_HARVESTER_H

#include <string>
#include <string_view>
#include <vector>
#include <angelscript.h>

namespace TokenHarvester
{

    /**
     * @struct LocalVariable
     * @brief Context storage for variables alive within current functional execution scopes.
     */
    struct LocalVariable
    {
        /** @brief The name of the local variable. */
        std::string name;
        /** @brief The type identifier for the variable. */
        std::string typeName;
        /** @brief Track curly brace nesting depth at instantiation time. */
        int declarationDepth;
    };

    /**
     * @struct GlobalFunction
     * @brief Context storage for globally declared functions, including their signatures for completion and hover info.
     */
    struct GlobalFunction
    {
        /** @brief The identifier name of the function. */
        std::string name;
        /** @brief The return type of the global function. */
        std::string typeName;
        /** @brief The full signature declaration of the function. */
        std::string declaration;
    };

    /**
     * @struct GlobalVariable
     * @brief Context storage for globally declared variables, including their types for completion and hover info.
     */
    struct GlobalVariable
    {
        /** @brief The identifier name of the global variable. */
        std::string name;
        /** @brief The data type of the global variable. */
        std::string typeName;
    };

    /**
     * @struct ClassProperty
     * @brief Context storage for class properties, including their types and access specifiers for completion.
     */
    struct ClassProperty
    {
        /** @brief The identifier name of the property. */
        std::string name;
        /** @brief The data type of the property. */
        std::string typeName;
        /** @brief The access specifier (e.g., "public", "private", "protected"). */
        std::string access;
    };

    /**
     * @struct ClassMethod
     * @brief Context storage for class methods, including their signatures and access specifiers for completion and hover info.
     */
    struct ClassMethod
    {
        /** @brief The identifier name of the method. */
        std::string name;
        /** @brief The return type of the method. */
        std::string typeName;
        /** @brief The full signature declaration of the method. */
        std::string declaration;
        /** @brief The access specifier (e.g., "public", "private", "protected"). */
        std::string access;
        /** @brief Flag indicating if this method serves as a constructor. */
        bool isConstructor;
    };

    /**
     * @struct ScriptClass
     * @brief Represents a user-defined class in the script, fully populated with its members.
     */
    struct ScriptClass
    {
        /** @brief The identifier name of the class. */
        std::string name;
        /** @brief List of base types for inheritance (e.g., "BaseClass", "Interface"). */
        std::vector<std::string> baseTypes;
        /** @brief Collection of properties encapsulated by the class. */
        std::vector<ClassProperty> properties;
        /** @brief Collection of methods defined within the class. */
        std::vector<ClassMethod> methods;
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
        /** @brief The absolute linear position of the cursor in the source code. */
        std::string lastSeparator;
    };

    /**
     * @brief Strips qualifiers and modifiers from a type string to get the base type for lookup purposes.
     * @param type The original type string, potentially containing qualifiers like "const" or handles "@".
     * @return The base type string stripped of any modifiers.
     */
    std::string GetBaseType(const std::string &type);

    /**
     * @brief Parses a data type from the token stream, handling user-defined classes and native types.
     * @param type The type string to parse.
     * @return The parsed inner type representation.
     */
    std::string ExtractInnerType(const std::string &type);

    /**
     * @brief Strips references and pointers but PRESERVES template arguments (<T>) for exact generic matching.
     * @param type The original type string, potentially containing references, pointers, and template arguments.
     * @return The instantiated type string with references and pointers removed but template arguments intact.
     */
    std::string GetInstantiatedType(const std::string &type);

    /**
     * @brief Evaluates if a token sequence represents a valid data type, including user-defined classes.
     * @param engine Pointer to the active AngelScript engine instance.
     * @param code The source code string view to be analyzed.
     * @return A vector containing all identified custom script classes.
     */
    std::vector<ScriptClass> ScanCustomClasses(asIScriptEngine *engine, std::string_view code);

    /**
     * @brief Parses backward from the cursor to determine if we're in a member access context and extracts relevant tokens.
     * @param engine Pointer to the active AngelScript engine instance.
     * @param code The source code string view being analyzed.
     * @param cursorAbsolutePos The linear index position of the cursor.
     * @return The structural completion context corresponding to the cursor's location.
     */
    CompletionContext GetCompletionContext(asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos);

    /**
     * @brief Computes multidimensional rows and columns down to a linear string offset character tracker.
     * @param text The full multi-line string view.
     * @param line The zero-based index of the target line.
     * @param character The zero-based index of the target character on the line.
     * @return The absolute linear position within the total string.
     */
    size_t GetAbsolutePosition(std::string_view text, int line, int character);

    /**
     * @brief Traverses tokens sequentially inside a scope to map active local variables and detect enclosing classes.
     * @param engine Pointer to the active AngelScript engine instance.
     * @param code The source code string view being analyzed.
     * @param cursorAbsolutePos The linear index position of the cursor defining the scope limit.
     * @param outEnclosingClass Output string parameter populated with the name of the active enclosing class, if any.
     * @return A vector of active local variables within the identified scope.
     */
    std::vector<LocalVariable> ScanLocalVariables(asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos, std::string &outEnclosingClass, const std::vector<ScriptClass> &customClasses, const std::vector<GlobalVariable> &globalVars, const std::vector<GlobalFunction> &globalFuncs);
    /**
     * @brief Extracts global function signatures using safe pattern matching token boundaries.
     * @param engine Pointer to the active AngelScript engine instance.
     * @param code The source code string view to be scanned.
     * @return A vector containing all globally defined functions.
     */
    std::vector<GlobalFunction> ScanGlobalFunctions(asIScriptEngine *engine, std::string_view code);

    /**
     * @brief Traverses tokens globally inside a raw file to extract global variable records.
     * @param engine Pointer to the active AngelScript engine instance.
     * @param code The source code string view to be scanned.
     * @return A vector containing all globally defined variables.
     */
    std::vector<GlobalVariable> ScanGlobalVariables(asIScriptEngine *engine, std::string_view code);
}

#endif // TOKEN_HARVESTER_H