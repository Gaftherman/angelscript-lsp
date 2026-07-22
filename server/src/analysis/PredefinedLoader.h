/**
 * @file PredefinedLoader.h
 * @brief Loader for workspace host application predefined declarations (as.predefined).
 * @ingroup Analysis
 */

#pragma once

#include <string>
#include <functional>
#include <angelscript.h>
#include "analysis/SymbolTable.h"

namespace analysis
{
    /**
     * @brief Utility class for loading predefined global symbols and host types into AST and AngelScript engine.
     * @note Thread-safe static loader helper.
     */
    class PredefinedLoader
    {
    public:
        /**
         * @brief Searches for as.predefined in the workspace root URI and loads declarations.
         *
         * @param[in] rootUri The workspace root URI string.
         * @param[in] engine Pointer to the AngelScript engine instance.
         * @param[out] table The symbol table to populate with predefined symbols.
         * @param[in] stringType Custom string type identifier (defaults to "string").
         * @param[in] arrayType Custom array type identifier (defaults to "array").
         * @param[in] logger Optional logger callback function.
         * @return bool True if as.predefined was found and successfully loaded, false otherwise.
         */
        static bool FindInWorkspace(const std::string &rootUri, asIScriptEngine *engine, SymbolTable &table, const std::string &stringType = "string", const std::string &arrayType = "array", std::function<void(const std::string &, int)> logger = nullptr);

        /**
         * @brief Loads host application definitions from an absolute file path.
         *
         * @param[in] filePath Absolute file path to the definitions file.
         * @param[in] engine Pointer to the AngelScript engine instance.
         * @param[out] table The symbol table to populate.
         * @param[in] stringType Custom string type identifier.
         * @param[in] arrayType Custom array type identifier.
         * @param[in] logger Optional logger callback function.
         * @return bool True if loaded successfully, false otherwise.
         */
        static bool LoadFromFile(const std::string &filePath, asIScriptEngine *engine, SymbolTable &table, const std::string &stringType = "string", const std::string &arrayType = "array", std::function<void(const std::string &, int)> logger = nullptr);

        /**
         * @brief Registers default standard types (string, array, dictionary, math functions) into engine and table.
         *
         * @param[in] engine Pointer to the AngelScript engine instance.
         * @param[out] table The symbol table to populate.
         */
        static void RegisterDefaultPredefined(asIScriptEngine *engine, SymbolTable &table);

        /**
         * @brief Loads definitions directly from source code string.
         *
         * @param[in] source Raw AngelScript header source text.
         * @param[in] engine Pointer to the AngelScript engine instance.
         * @param[out] table The symbol table to populate.
         * @param[in] stringType Custom string type identifier.
         * @param[in] arrayType Custom array type identifier.
         * @param[in] logger Optional logger callback function.
         * @param[in] customUri Custom URI identifier for the loaded document.
         * @return bool True if loaded successfully, false otherwise.
         */
        static bool LoadFromSource(const std::string &source, asIScriptEngine *engine, SymbolTable &table, const std::string &stringType = "string", const std::string &arrayType = "array", std::function<void(const std::string &, int)> logger = nullptr, const std::string &customUri = "file:///as.predefined");
    };

} // namespace analysis
