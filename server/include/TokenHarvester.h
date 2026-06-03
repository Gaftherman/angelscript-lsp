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

namespace TokenHarvester {
    /**
     * @struct LocalVariable
     * @brief Context storage for variables alive within current functional execution scopes.
     */
    struct LocalVariable {
        std::string name;
        std::string typeName;
        int declarationDepth; ///< Track curly brace nesting depth at instantiation time
    };

    /**
     * @struct GlobalFunction
     * @brief Context storage for globally declared functions, including their signatures for completion and hover info
     */
    struct GlobalFunction {
        std::string name;
        std::string returnType;
        std::string declaration;
    };

    /**
     * @struct GlobalVariable
     * @brief Context storage for globally declared variables, including their types for completion and hover info
     */
    struct GlobalVariable {
        std::string name;
        std::string typeName;
    };

    /**
     * @brief Computes multidimensional rows and columns down to a linear string offset character tracker.
     */
    size_t GetAbsolutePosition(std::string_view text, int line, int character);

    /**
     * @brief Traverses tokens sequentially inside a scope to map active local variables and detect enclosing classes.
     */
    std::vector<LocalVariable> ScanLocalVariables(asIScriptEngine* engine, std::string_view code, size_t cursorAbsolutePos, std::string& outEnclosingClass);

    /**
     * @brief Extracts global function signatures using safe pattern matching token boundaries.
     */
    std::vector<GlobalFunction> ScanGlobalFunctions(asIScriptEngine* engine, std::string_view code);

    /**
     * @brief Traverses tokens globally inside a raw file to extract global variable records.
     */
    std::vector<GlobalVariable> ScanGlobalVariables(asIScriptEngine* engine, std::string_view code);
}

#endif // TOKEN_HARVESTER_H