/**
 * @file PredefinedLoader.h
 * @brief Loader for workspace host application predefined declarations (as.predefined).
 * @ingroup Analysis
 */

#pragma once

#include <string>
#include <string_view>
#include <functional>
#include "analysis/SymbolTable.h"

namespace analysis
{
    /**
     * @brief Utility class for loading predefined global symbols and host types into AST SymbolTable.
     * @note Thread-safe static loader helper.
     */
    class PredefinedLoader
    {
    public:
        /**
         * @brief Searches for as.predefined in the workspace root URI and loads declarations into SymbolTable.
         *
         * @param[in] rootUri The workspace root URI string.
         * @param[out] table The symbol table to populate with predefined symbols.
         * @param[in] stringType Custom string type identifier (defaults to "string").
         * @param[in] arrayType Custom array type identifier (defaults to "array").
         * @param[in] logger Optional logger callback function.
         * @return bool True if as.predefined was found and successfully loaded, false otherwise.
         */
        static bool FindInWorkspace(std::string_view rootUri, SymbolTable &table, std::string_view stringType = "string", std::string_view arrayType = "array", std::function<void(const std::string &, int)> logger = nullptr);

        /**
         * @brief Loads host application definitions from an absolute file path.
         *
         * @param[in] filePath Absolute file path to the definitions file.
         * @param[out] table The symbol table to populate.
         * @param[in] stringType Custom string type identifier.
         * @param[in] arrayType Custom array type identifier.
         * @param[in] logger Optional logger callback function.
         * @return bool True if loaded successfully, false otherwise.
         */
        static bool LoadFromFile(std::string_view filePath, SymbolTable &table, std::string_view stringType = "string", std::string_view arrayType = "array", std::function<void(const std::string &, int)> logger = nullptr);

        /**
         * @brief Loads definitions directly from source code string.
         *
         * @param[in] source Raw AngelScript header source text.
         * @param[out] table The symbol table to populate.
         * @param[in] stringType Custom string type identifier.
         * @param[in] arrayType Custom array type identifier.
         * @param[in] logger Optional logger callback function.
         * @param[in] customUri Custom URI identifier for the loaded document.
         * @return bool True if loaded successfully, false otherwise.
         */
        static bool LoadFromSource(std::string_view source, SymbolTable &table, std::string_view stringType = "string", std::string_view arrayType = "array", std::function<void(const std::string &, int)> logger = nullptr, std::string_view customUri = "file:///as.predefined");
    };

} // namespace analysis
