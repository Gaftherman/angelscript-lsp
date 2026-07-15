#pragma once

#include <string>
#include <angelscript.h>

namespace analysis {

class PredefinedLoader {
public:
    // Finds as.predefined in the workspace root URI and loads it if it exists
    static bool FindInWorkspace(const std::string& rootUri, asIScriptEngine* engine);

    // Loads the JSON file at the given absolute path and registers its types/functions in the engine
    static bool LoadAndRegister(const std::string& jsonFilePath, asIScriptEngine* engine);

private:
    static void RegisterTypes(const std::string& jsonData, asIScriptEngine* engine);
};

} // namespace analysis
