#pragma once

#include <string>
#include <functional>
#include <angelscript.h>
#include "analysis/SymbolTable.h"

namespace analysis
{
    /**
     * @brief Utility for loading predefined globals and types into the AST and AngelScript engine.
     */
    class PredefinedLoader
    {
    public:
        /**
         * @brief Finds as.predefined in the workspace root URI and loads it if it exists.
         *
         * @param rootUri The workspace root URI.
         * @param engine The AngelScript engine instance.
         * @param table The symbol table to populate.
         * @param stringType The string type name.
         * @param arrayType The array type name.
         * @param logger An optional logger callback.
         * @return true if successfully found and loaded, false otherwise.
         */
        static bool FindInWorkspace(const std::string &rootUri, asIScriptEngine *engine, SymbolTable &table, const std::string &stringType = "string", const std::string &arrayType = "array", std::function<void(const std::string &, int)> logger = nullptr);

        /**
         * @brief Loads definitions from the given absolute path.
         *
         * @param filePath The absolute file path to the definitions file.
         * @param engine The AngelScript engine instance.
         * @param table The symbol table to populate.
         * @param stringType The string type name.
         * @param arrayType The array type name.
         * @param logger An optional logger callback.
         * @return true if loaded successfully, false otherwise.
         */
        static bool LoadFromFile(const std::string &filePath, asIScriptEngine *engine, SymbolTable &table, const std::string &stringType = "string", const std::string &arrayType = "array", std::function<void(const std::string &, int)> logger = nullptr);

        /**
         * @brief Loads definitions from source text.
         *
         * @param source The source text.
         * @param engine The AngelScript engine instance.
         * @param table The symbol table to populate.
         * @param stringType The string type name.
         * @param arrayType The array type name.
         * @param logger An optional logger callback.
         * @return true if loaded successfully, false otherwise.
         */
        static bool LoadFromSource(const std::string &source, asIScriptEngine *engine, SymbolTable &table, const std::string &stringType = "string", const std::string &arrayType = "array", std::function<void(const std::string &, int)> logger = nullptr);
    };

} // namespace analysis
