#include "PredefinedLoader.h"
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace analysis {

bool PredefinedLoader::FindInWorkspace(const std::string& rootUri, asIScriptEngine* engine) {
    if (rootUri.empty()) return false;
    
    // Simplistic URI to path conversion for 'file://'
    std::string path = rootUri;
    if (path.find("file://") == 0) {
        path = path.substr(7);
        // Handle Windows paths like file:///C:/...
        if (path.length() > 0 && path[0] == '/') {
            path = path.substr(1);
        }
    }
    
    std::string fullPath = path + "/as.predefined";
    return LoadAndRegister(fullPath, engine);
}

bool PredefinedLoader::LoadAndRegister(const std::string& jsonFilePath, asIScriptEngine* engine) {
    if (!engine) return false;

    std::ifstream file(jsonFilePath);
    if (!file.is_open()) {
        spdlog::warn("Could not open predefined file: {}", jsonFilePath);
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    
    try {
        RegisterTypes(buffer.str(), engine);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse predefined JSON: {}", e.what());
        return false;
    }
}

void PredefinedLoader::RegisterTypes(const std::string& jsonData, asIScriptEngine* engine) {
    json root = json::parse(jsonData);

    // 1. Enums
    if (root.contains("enums")) {
        for (const auto& en : root["enums"]) {
            std::string name = en["name"];
            engine->RegisterEnum(name.c_str());
            // We could register enum values, but the headless compiler mainly cares about the type existence
            if (en.contains("values")) {
                for (const auto& val : en["values"]) {
                    std::string valName = val["name"];
                    // engine->RegisterEnumValue(name.c_str(), valName.c_str(), value);
                }
            }
        }
    }

    // 2. Funcdefs
    if (root.contains("funcdefs")) {
        for (const auto& fd : root["funcdefs"]) {
            std::string decl = fd["decl"];
            engine->RegisterFuncdef(decl.c_str());
        }
    }

    // 3. Types (Classes / Interfaces)
    if (root.contains("types")) {
        for (const auto& t : root["types"]) {
            std::string name = t["name"];
            
            int flags = asOBJ_REF; // Default to reference type
            if (t.contains("flags")) {
                for (const auto& f : t["flags"]) {
                    if (f == "value") flags = asOBJ_VALUE | asOBJ_POD;
                }
            }

            // Register the object type
            int r = engine->RegisterObjectType(name.c_str(), 0, flags);
            if (r < 0) {
                spdlog::warn("Failed to register object type: {} (error {})", name, r);
                continue;
            }

            // Register methods
            if (t.contains("methods")) {
                for (const auto& m : t["methods"]) {
                    std::string decl = m["decl"];
                    // We use asFUNCTION(0) (a null pointer) because we will never EXECUTE the code,
                    // we only compile it for validation/diagnostics.
                    engine->RegisterObjectMethod(
                        name.c_str(), 
                        decl.c_str(), 
                        asFUNCTION(0), 
                        asCALL_CDECL_OBJFIRST
                    );
                }
            }

            // Register properties
            if (t.contains("properties")) {
                for (const auto& p : t["properties"]) {
                    std::string decl = p["decl"];
                    // Note: byte offset doesn't matter for pure validation
                    engine->RegisterObjectProperty(name.c_str(), decl.c_str(), 0);
                }
            }
        }
    }

    // 4. Global Functions
    if (root.contains("functions")) {
        for (const auto& f : root["functions"]) {
            std::string decl = f["decl"];
            engine->RegisterGlobalFunction(decl.c_str(), asFUNCTION(0), asCALL_CDECL);
        }
    }
    
    // 5. Global Properties
    if (root.contains("properties")) {
        for (const auto& p : root["properties"]) {
            std::string decl = p["decl"];
            // We need a dummy pointer for global properties
            static int dummyVar = 0;
            engine->RegisterGlobalProperty(decl.c_str(), &dummyVar);
        }
    }
}

} // namespace analysis
