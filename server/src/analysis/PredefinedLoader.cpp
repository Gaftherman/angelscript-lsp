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

    // Helper for recursive processing
    auto processSymbols = [&](auto self, const std::vector<std::shared_ptr<Symbol>>& symbols, int pass) -> void {
        for (const auto& sym : symbols)
        {
            if (sym->kind == SymbolKind::Namespace)
            {
                std::string oldNs = engine->GetDefaultNamespace();
                engine->SetDefaultNamespace(sym->name.c_str());
                self(self, sym->children, pass);
                engine->SetDefaultNamespace(oldNs.c_str());
            }
            else if (pass == 1)
            {
                if (sym->kind == SymbolKind::Enum)
                {
                    if (registeredTypes.find(sym->name) != registeredTypes.end()) continue;
                    registeredTypes.insert(sym->name);
                    engine->RegisterEnum(sym->name.c_str());
                    // Register enum members
                    for (const auto& child : sym->children) {
                        if (child->kind == SymbolKind::EnumMember) {
                            engine->RegisterEnumValue(sym->name.c_str(), child->name.c_str(), 0);
                        }
                    }
                }
                else if (sym->kind == SymbolKind::Typedef)
                {
                    if (registeredTypes.find(sym->name) != registeredTypes.end()) continue;
                    registeredTypes.insert(sym->name);
                    engine->RegisterTypedef(sym->name.c_str(), sym->typeInfo.c_str());
                }
                else if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Interface || sym->kind == SymbolKind::Mixin)
                {
                    if (registeredTypes.find(sym->name) != registeredTypes.end()) continue;
                    registeredTypes.insert(sym->name);
                    
                    int flags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_CDAK | asOBJ_APP_CLASS_ALLINTS | asOBJ_APP_CLASS_ALLFLOATS;
                    int size = 4;
                    std::string declName = sym->name;
                    std::string registerName = declName;
                    
                    if (sym->name == arrayType || sym->name == arrayType + "<T>") {
                        declName = arrayType + "<T>";
                        registerName = arrayType + "<class T>";
                        flags = asOBJ_REF | asOBJ_NOCOUNT | asOBJ_TEMPLATE;
                        size = 0;
                    }
                    
                    engine->RegisterObjectType(registerName.c_str(), size, flags);
                    
                    if (sym->name == stringType) {
                        engine->RegisterStringFactory(stringType.c_str(), &g_dummyStringFactory);
                    }
                }
            }
            else if (pass == 2)
            {
                if (sym->kind == SymbolKind::Funcdef)
                {
                    if (registeredTypes.find(sym->name) != registeredTypes.end()) continue;
                    registeredTypes.insert(sym->name);
                    engine->RegisterFuncdef(sym->signature.c_str());
                }
            }
            else if (pass == 3)
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
                            engine->RegisterObjectMethod(declName.c_str(), child->signature.c_str(), asFUNCTION(0), asCALL_GENERIC);
                        }
                        else if (child->kind == SymbolKind::Property)
                        {
                            engine->RegisterObjectProperty(declName.c_str(), child->signature.c_str(), 0);
                        }
                    }
                }
                else if (sym->kind == SymbolKind::Function)
                {
                    engine->RegisterGlobalFunction(sym->signature.c_str(), asFUNCTION(0), asCALL_GENERIC);
                }
                else if (sym->kind == SymbolKind::Variable)
                {
                    static int dummyVar = 0;
                    engine->RegisterGlobalProperty(sym->signature.c_str(), &dummyVar);
                }
            }
        }
    };

    // Flatten globals
    std::vector<std::shared_ptr<Symbol>> globals;
    for (const auto& [name, overloads] : table.GetGlobals()) {
        for (const auto& sym : overloads) {
            globals.push_back(sym);
        }
    }

    // Pass 1: Types
    processSymbols(processSymbols, globals, 1);
    
    // Register Default Array ONLY if it was registered
    if (engine->GetTypeInfoByName((arrayType + "<T>").c_str()) != nullptr || 
        engine->GetTypeInfoByName(arrayType.c_str()) != nullptr) {
        engine->RegisterDefaultArrayType((arrayType + "<T>").c_str());
    }

    // Pass 2: Funcdefs
    processSymbols(processSymbols, globals, 2);

    // Pass 3: Methods, properties, functions
    processSymbols(processSymbols, globals, 3);

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
