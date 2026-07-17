#pragma once

#include <string>
#include <functional>
#include <angelscript.h>
#include "analysis/SymbolTable.h"

namespace analysis {

class PredefinedLoader {
public:
    // Finds as.predefined in the workspace root URI and loads it if it exists
    static bool FindInWorkspace(const std::string& rootUri, asIScriptEngine* engine, SymbolTable& table, const std::string& stringType = "string", const std::string& arrayType = "array", std::function<void(const std::string&, int)> logger = nullptr);

    // Loads from the given absolute path
    static bool LoadFromFile(const std::string& filePath, asIScriptEngine* engine, SymbolTable& table, const std::string& stringType = "string", const std::string& arrayType = "array", std::function<void(const std::string&, int)> logger = nullptr);

    // Loads from source text
    static bool LoadFromSource(const std::string& source, asIScriptEngine* engine, SymbolTable& table, const std::string& stringType = "string", const std::string& arrayType = "array", std::function<void(const std::string&, int)> logger = nullptr);
};

} // namespace analysis
