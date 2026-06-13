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
#include <unordered_map>
#include <angelscript.h>

namespace TokenHarvester
{
    /**
     * @struct LocalVariable
     * @brief Context storage for variables alive within current functional execution scopes.
     */
    struct LocalVariable
    {
        std::string name;
        std::string typeName;
        int declarationDepth;
    };

    /**
     * @struct GlobalFunction
     * @brief Context storage for globally declared functions.
     */
    struct GlobalFunction
    {
        std::string name;
        std::string typeName;
        std::string declaration;
    };

    /**
     * @struct GlobalVariable
     * @brief Context storage for globally declared variables.
     */
    struct GlobalVariable
    {
        std::string name;
        std::string typeName;
    };

    /**
     * @struct ClassProperty
     * @brief Context storage for class properties, including access specifiers.
     */
    struct ClassProperty
    {
        std::string name;
        std::string typeName;
        std::string access;
    };

    /**
     * @struct ClassMethod
     * @brief Context storage for class methods, tracking signatures and overloads.
     */
    struct ClassMethod
    {
        std::string name;
        std::string typeName;
        std::string declaration;
        std::string access;
        bool isConstructor;
    };

    /**
     * @struct ScriptClass
     * @brief Represents a user-defined class fully populated with O(1) associative lookup tables.
     */
    struct ScriptClass
    {
        std::string name;
        std::vector<std::string> baseTypes;
        std::unordered_map<std::string, ClassProperty> properties;
        std::unordered_map<std::string, std::vector<ClassMethod>> methods;
    };

    /**
     * @struct CompletionContext
     * @brief Natively resolved context for autocompletion triggering.
     */
    struct CompletionContext
    {
        bool isMemberAccess;
        std::vector<std::string> objectChain;
        std::string partialMember;
        std::string lastSeparator;
    };

    /**
     * @brief Strips qualifiers and modifiers from a type string to get the base type.
     * @param type The original type string view.
     * @return The base type string view stripped of modifiers.
     */
    std::string_view GetBaseType(std::string_view type) noexcept;

    /**
     * @brief Extracts the underlying parameter type wrapped inside collections.
     * @param type The raw data type string view.
     * @return The isolated inner data type representation string view.
     */
    std::string_view ExtractInnerType(std::string_view type) noexcept;

    /**
     * @brief Strips references and pointers but preserves template arguments (<T>).
     * @param type The original type string view.
     * @return The instantiated type string view.
     */
    std::string_view GetInstantiatedType(std::string_view type) noexcept;

    /**
     * @brief Evaluates if a token sequence represents a valid data type.
     * @param engine Pointer to the active AngelScript engine instance.
     * @param code The source code string view to be analyzed.
     * @return A vector containing all identified custom script classes.
     */
    std::vector<ScriptClass> ScanCustomClasses(asIScriptEngine *engine, std::string_view code);

    /**
     * @brief Parses backward from the cursor to determine completion contexts.
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
     * @brief Traverses tokens sequentially inside a scope to map active local variables.
     * @param engine Pointer to the active AngelScript engine instance.
     * @param code The source code string view being analyzed.
     * @param cursorAbsolutePos The linear index position of the cursor defining the scope limit.
     * @param outEnclosingClass Output string parameter populated with the active enclosing class name.
     * @param customClasses Mapped script classes metadata database profiles.
     * @param globalVars Collection summarizing file layer variable properties.
     * @param globalFuncs Registry mapping globally compiled operations methods routines.
     * @return A vector of active local variables within the identified scope.
     */
    std::vector<LocalVariable> ScanLocalVariables(asIScriptEngine *engine, std::string_view code, size_t cursorAbsolutePos, std::string &outEnclosingClass, const std::vector<ScriptClass> &customClasses, const std::vector<GlobalVariable> &globalVars, const std::vector<GlobalFunction> &globalFuncs);

    /**
     * @brief Extracts global function signatures using safe pattern matching.
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