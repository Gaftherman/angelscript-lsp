#include "PredefinedLoader.h"
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include "document/Document.h"
#include "analysis/SymbolCollector.h"

namespace analysis
{

class DummyStringFactory : public asIStringFactory {
public:
    const void* GetStringConstant(const char*, asUINT) override { 
        static int dummyVal = 1;
        return &dummyVal; 
    }
    int ReleaseStringConstant(const void*) override { return 0; }
    int GetRawStringData(const void*, char*, asUINT* length) const override { 
        if (length) *length = 0;
        return 0; 
    }
};
static DummyStringFactory g_dummyStringFactory;

static void PredefinedMessageCallback(const asSMessageInfo *msg, void *param)
{
    auto* logger = static_cast<std::function<void(const std::string&, int)>*>(param);
    
    std::string formatted = "AS PREDEFINED ";
    int severity = 2;
    if (msg->type == asMSGTYPE_ERROR) {
        formatted += "ERROR";
        severity = 1; // Error in LSP
        spdlog::error("AS PREDEFINED ERROR: {} (Row: {})", msg->message, msg->row);
    } else if (msg->type == asMSGTYPE_WARNING) {
        formatted += "WARN";
        severity = 2; // Warn in LSP
        spdlog::warn("AS PREDEFINED WARN: {} (Row: {})", msg->message, msg->row);
    } else {
        formatted += "INFO";
        severity = 3; // Info in LSP
        spdlog::info("AS PREDEFINED INFO: {} (Row: {})", msg->message, msg->row);
    }
    
    formatted += ": " + std::string(msg->message) + " (Row: " + std::to_string(msg->row) + ")";
    
    if (logger && *logger) {
        (*logger)(formatted, severity);
    }
}

// Helper to register symbols from the symbol table into the AS engine
static void RegisterSymbols(const SymbolTable& table, asIScriptEngine* engine, const std::string& stringType, const std::string& arrayType, std::function<void(const std::string&, int)>* logger)
{
    // We can't easily get the old callback, but PredefinedLoader is called when engine is fresh or under our control.
    engine->SetMessageCallback(asFUNCTION(PredefinedMessageCallback), logger, asCALL_CDECL_OBJLAST);

    std::unordered_set<std::string> registeredTypes;

    // Step 1: Register all enums, classes, and interfaces FIRST
    for (const auto& [name, overloads] : table.GetGlobals())
    {
        for (const auto& sym : overloads)
        {
            if (sym->kind == SymbolKind::Enum)
            {
                if (registeredTypes.find(sym->name) != registeredTypes.end()) continue;
                registeredTypes.insert(sym->name);
                engine->RegisterEnum(sym->name.c_str());
            }
            else if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Interface || sym->kind == SymbolKind::Mixin)
            {
                if (registeredTypes.find(sym->name) != registeredTypes.end()) continue;
                registeredTypes.insert(sym->name);
                
                // Register as a value type with a dummy size so it can be instantiated by value
                int flags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_CDAK | asOBJ_APP_CLASS_ALLINTS | asOBJ_APP_CLASS_ALLFLOATS;
                int size = 4;
                std::string declName = sym->name;
                
                std::string registerName = declName;
                if (sym->name == arrayType || sym->name == arrayType + "<T>") {
                    declName = arrayType + "<T>";
                    registerName = arrayType + "<class T>";
                    // Templates cannot be POD, so they must be reference types.
                    flags = asOBJ_REF | asOBJ_NOCOUNT | asOBJ_TEMPLATE;
                    size = 0;
                }

                engine->RegisterObjectType(registerName.c_str(), size, flags);

                // If this is the string type, register the dummy string factory
                if (sym->name == stringType) {
                    engine->RegisterStringFactory(stringType.c_str(), &g_dummyStringFactory);
                }
            }
        }
    }

    // Register Default Array ONLY if it was registered
    if (engine->GetTypeInfoByName((arrayType + "<T>").c_str()) != nullptr || 
        engine->GetTypeInfoByName(arrayType.c_str()) != nullptr) {
        engine->RegisterDefaultArrayType((arrayType + "<T>").c_str());
    }

    // Step 2: Register funcdefs (now that all types exist)
    for (const auto& [name, overloads] : table.GetGlobals())
    {
        for (const auto& sym : overloads)
        {
            if (sym->kind == SymbolKind::Funcdef)
            {
                if (registeredTypes.find(sym->name) != registeredTypes.end()) continue;
                registeredTypes.insert(sym->name);
                engine->RegisterFuncdef(sym->signature.c_str());
            }
        }
    }

    // Step 3: Register methods, properties, and global functions

    for (const auto& [name, overloads] : table.GetGlobals())
    {
        for (const auto& sym : overloads)
        {
            if (sym->kind == SymbolKind::Enum)
            {
                engine->RegisterEnum(sym->name.c_str());
            }
            else if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Interface || sym->kind == SymbolKind::Mixin)
            {
                // Register as a value type with a dummy size so it can be instantiated by value
                // e.g. `string s;` instead of `string@ s;`
                int flags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_CDAK;
                int size = 4;
                std::string declName = sym->name;
                
                std::string registerName = declName;
                if (sym->name == arrayType || sym->name == arrayType + "<T>") {
                    declName = arrayType + "<T>";
                    registerName = arrayType + "<class T>";
                    // Templates cannot be POD, so they must be reference types.
                    flags = asOBJ_REF | asOBJ_NOCOUNT | asOBJ_TEMPLATE;
                    size = 0;
                }

                engine->RegisterObjectType(registerName.c_str(), size, flags);

                // If this is the string type, register the dummy string factory
                if (sym->name == stringType) {
                    engine->RegisterStringFactory(stringType.c_str(), &g_dummyStringFactory);
                }
            }
        }
    }

    // Register Default Array ONLY if it was registered
    if (engine->GetTypeInfoByName((arrayType + "<T>").c_str()) != nullptr || 
        engine->GetTypeInfoByName(arrayType.c_str()) != nullptr) {
        engine->RegisterDefaultArrayType((arrayType + "<T>").c_str());
    }

    // Step 2: Register methods and properties for all classes
    for (const auto& [name, overloads] : table.GetGlobals())
    {
        for (const auto& sym : overloads)
        {
            if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Interface || sym->kind == SymbolKind::Mixin)
            {
                std::string declName = sym->name;
                if (sym->name == arrayType || sym->name == arrayType + "<T>") {
                    declName = arrayType + "<T>";
                }

                for (const auto& child : sym->children)
                {
                    if (child->kind == SymbolKind::Method)
                    {
                        engine->RegisterObjectMethod(
                            declName.c_str(),
                            child->signature.c_str(),
                            asFUNCTION(0),
                            asCALL_CDECL_OBJFIRST
                        );
                    }
                    else if (child->kind == SymbolKind::Property)
                    {
                        engine->RegisterObjectProperty(declName.c_str(), child->signature.c_str(), 0);
                    }
                }
            }
            else if (sym->kind == SymbolKind::Function)
            {
                engine->RegisterGlobalFunction(sym->signature.c_str(), asFUNCTION(0), asCALL_CDECL);
            }
            else if (sym->kind == SymbolKind::Variable)
            {
                // Variables as global properties
                static int dummyVar = 0;
                engine->RegisterGlobalProperty(sym->signature.c_str(), &dummyVar);
            }
        }
    }

    engine->ClearMessageCallback();
}

bool PredefinedLoader::LoadFromSource(const std::string& source, asIScriptEngine* engine, SymbolTable& table, const std::string& stringType, const std::string& arrayType, std::function<void(const std::string&, int)> logger)
{
    if (!engine) return false;

    // Use a temporary document to parse the source
    Document doc("file:///as.predefined", source);
    SymbolCollector::CollectGlobals(doc, table);

    // Call RegisterSymbols passing the logger pointer
    RegisterSymbols(table, engine, stringType, arrayType, &logger);

    return true;
}

bool PredefinedLoader::LoadFromFile(const std::string& filePath, asIScriptEngine* engine, SymbolTable& table, const std::string& stringType, const std::string& arrayType, std::function<void(const std::string&, int)> logger)
{
    std::ifstream file(filePath);
    if (!file.is_open()) return false;

    std::stringstream buffer;
    buffer << file.rdbuf();

    return LoadFromSource(buffer.str(), engine, table, stringType, arrayType, logger);
}

bool PredefinedLoader::FindInWorkspace(const std::string& rootUri, asIScriptEngine* engine, SymbolTable& table, const std::string& stringType, const std::string& arrayType, std::function<void(const std::string&, int)> logger)
{
    if (rootUri.empty()) return false;

    std::string path = rootUri;
    if (path.find("file://") == 0)
    {
        path = path.substr(7);
        if (path.length() > 0 && path[0] == '/')
        {
            path = path.substr(1);
        }
    }

    std::string fullPath = path + "/as.predefined";
    return LoadFromFile(fullPath, engine, table, stringType, arrayType, logger);
}

} // namespace analysis
