#include "PredefinedLoader.h"
#include <fstream>
#include <sstream>
#include <unordered_set>
#include "utils/LspLogger.h"
#include "document/Document.h"
#include "analysis/SymbolCollector.h"
#include <as_objecttype.h>

#ifdef max
#undef max
#endif

#ifdef min
#undef min
#endif

namespace analysis
{

    class DummyStringFactory : public asIStringFactory
    {
    public:
        const void *GetStringConstant(const char *, asUINT) override
        {
            static int dummyVal = 1;
            return &dummyVal;
        }
        int ReleaseStringConstant(const void *) override { return 0; }
        int GetRawStringData(const void *, char *, asUINT *length) const override
        {
            if (length)
                *length = 0;
            return 0;
        }
    };
    static DummyStringFactory g_dummyStringFactory;

    static int g_dummyObj = 1;
    static void DummyGeneric(asIScriptGeneric *gen)
    {
        // Return a dummy pointer for any object return types to prevent crashes
        int typeId = gen->GetReturnTypeId();
        if (typeId != asTYPEID_VOID)
        {
            if ((typeId & asTYPEID_OBJHANDLE) || (typeId & asTYPEID_SCRIPTOBJECT) || (typeId & asTYPEID_MASK_OBJECT))
            {
                gen->SetReturnAddress(&g_dummyObj);
            }
            else
            {
                asQWORD zero = 0;
                gen->SetReturnQWord(zero);
            }
        }
    }

    static void PredefinedMessageCallback(const asSMessageInfo *msg, void *param)
    {
        auto *logger = static_cast<std::function<void(const std::string &, int)> *>(param);

        std::string formatted = "AS PREDEFINED ";
        int severity = 2;
        if (msg->type == asMSGTYPE_ERROR)
        {
            formatted += "ERROR";
            // Downgrade to warning in LSP so we don't spam errors for invalid predefined files
            severity = 2;
            angel_lsp::LspLogger::Warn("AS PREDEFINED ERROR: " + std::string(msg->message) + " (Row: " + std::to_string(msg->row) + ")");
        }
        else if (msg->type == asMSGTYPE_WARNING)
        {
            formatted += "WARN";
            severity = 2; // Warn in LSP
            angel_lsp::LspLogger::Warn("AS PREDEFINED WARN: " + std::string(msg->message) + " (Row: " + std::to_string(msg->row) + ")");
        }
        else
        {
            formatted += "INFO";
            severity = 3; // Info in LSP
            angel_lsp::LspLogger::Info("AS PREDEFINED INFO: " + std::string(msg->message) + " (Row: " + std::to_string(msg->row) + ")");
        }

        formatted += ": " + std::string(msg->message) + " (Row: " + std::to_string(msg->row) + ")";

        if (logger && *logger)
        {
            (*logger)(formatted, severity);
        }
    }

    // Helper to register symbols from the symbol table into the AS engine
    static void RegisterSymbols(const SymbolTable &table, asIScriptEngine *engine, const std::string &stringType, const std::string &arrayType, std::function<void(const std::string &, int)> *logger)
    {
        // We can't easily get the old callback, but PredefinedLoader is called when engine is fresh or under our control.
        engine->SetMessageCallback(asFUNCTION(PredefinedMessageCallback), logger, asCALL_CDECL_OBJLAST);

        std::unordered_set<std::string> registeredTypes;

        // Helper for recursive processing
        auto processSymbols = [&](auto self, const std::vector<std::shared_ptr<Symbol>> &symbols, int pass) -> void
        {
            for (const auto &sym : symbols)
            {
                if (sym->kind == SymbolKind::Namespace)
                {
                    std::string oldNs = engine->GetDefaultNamespace();
                    std::string newNs = oldNs.empty() ? sym->name : oldNs + "::" + sym->name;
                    engine->SetDefaultNamespace(newNs.c_str());
                    self(self, sym->children, pass);
                    engine->SetDefaultNamespace(oldNs.c_str());
                }
                else if (pass == 1)
                {
                    if (sym->kind == SymbolKind::Enum)
                    {
                        if (registeredTypes.find(sym->name) != registeredTypes.end())
                            continue;
                        registeredTypes.insert(sym->name);
                        engine->RegisterEnum(sym->name.c_str());
                        // Register enum members
                        for (const auto &child : sym->children)
                        {
                            if (child->kind == SymbolKind::EnumMember)
                            {
                                engine->RegisterEnumValue(sym->name.c_str(), child->name.c_str(), 0);
                            }
                        }
                    }
                    else if (sym->kind == SymbolKind::Typedef)
                    {
                        if (registeredTypes.find(sym->name) != registeredTypes.end())
                            continue;
                        registeredTypes.insert(sym->name);
                        engine->RegisterTypedef(sym->name.c_str(), sym->typeInfo.c_str());
                    }
                    else if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Interface || sym->kind == SymbolKind::Mixin)
                    {
                        if (registeredTypes.find(sym->name) != registeredTypes.end())
                            continue;

                        int flags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_CDAK | asOBJ_APP_CLASS_ALLINTS | asOBJ_APP_CLASS_ALLFLOATS;
                        int size = 4;
                        std::string declName = sym->name;
                        std::string registerName = sym->name;

                        if (sym->isAbstract || sym->isShared || sym->kind == SymbolKind::Mixin || sym->kind == SymbolKind::Interface)
                        {
                            // Skip C++ registration for abstract/shared/mixin/interface types.
                            // They will be dynamically compiled as script classes in ValidationOracle.
                            continue;
                        }
                        else if (sym->name == arrayType || sym->name == arrayType + "<T>")
                        {
                            declName = arrayType + "<T>";
                            registerName = arrayType + "<class T>";
                            flags = asOBJ_REF | asOBJ_TEMPLATE;
                            size = 0;
                        }
                        else
                        {
                            // NOTE: We MUST register non-templates (like string, char, custom classes)
                            // as asOBJ_VALUE. If we register them as asOBJ_REF, any dummy methods that return
                            // them by value (e.g. `string opAdd(...) const`) will be rejected by AngelScript
                            // with asINVALID_DECLARATION, which permanently invalidates the engine configuration
                            // and breaks all subsequent LSP compilation.
                            flags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_CDAK | asOBJ_APP_CLASS_ALLINTS | asOBJ_APP_CLASS_ALLFLOATS;
                            size = 4;
                        }

                        int r = engine->RegisterObjectType(registerName.c_str(), size, flags);
                        if (r < 0)
                            continue;

                        registeredTypes.insert(sym->name);

                        // --- INHERITANCE NOTE ---
                        // Natively, AngelScript compiler forbids inheritance from C++ registered types.
                        // We previously injected asOBJ_SCRIPT_OBJECT here, but that causes SIGSEGV
                        // in vanilla AngelScript when building derived classes.
                        // We must leave it as a native type to prevent crashes.

                        if (flags & asOBJ_TEMPLATE)
                        {
                            // Register dummy memory management so handles and factories work for templates
                            engine->RegisterObjectBehaviour(declName.c_str(), asBEHAVE_ADDREF, "void f()", asFUNCTION(DummyGeneric), asCALL_GENERIC);
                            engine->RegisterObjectBehaviour(declName.c_str(), asBEHAVE_RELEASE, "void f()", asFUNCTION(DummyGeneric), asCALL_GENERIC);

                            // Register a dummy factory so we can instantiate this type locally by value
                            std::string factorySig = declName + "@ f(int&in)";
                            engine->RegisterObjectBehaviour(declName.c_str(), asBEHAVE_FACTORY, factorySig.c_str(), asFUNCTION(DummyGeneric), asCALL_GENERIC);

                            // Register dummy list factory for initialization lists like array<int> a = {1, 2, 3};
                            std::string listFactorySig = declName + "@ f(int&in, int&in) {repeat T}";
                            engine->RegisterObjectBehaviour(declName.c_str(), asBEHAVE_LIST_FACTORY, listFactorySig.c_str(), asFUNCTION(DummyGeneric), asCALL_GENERIC);
                        }

                        if (sym->name == stringType)
                        {
                            engine->RegisterStringFactory(stringType.c_str(), &g_dummyStringFactory);
                        }
                    }
                }
                else if (pass == 2)
                {
                    if (sym->kind == SymbolKind::Funcdef)
                    {
                        if (registeredTypes.find(sym->name) != registeredTypes.end())
                            continue;
                        registeredTypes.insert(sym->name);
                        engine->RegisterFuncdef(sym->BuildSignature().c_str());
                    }
                }
                else if (pass == 3)
                {
                    if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Interface || sym->kind == SymbolKind::Mixin)
                    {
                        if (registeredTypes.find(sym->name) == registeredTypes.end())
                            continue;

                        std::string declName = sym->name;
                        if (sym->name == arrayType || sym->name == arrayType + "<T>")
                        {
                            declName = arrayType + "<T>";
                        }

                        for (const auto &child : sym->children)
                        {
                            if (child->kind == SymbolKind::Method)
                            {
                                // Do not register constructors or destructors as object methods.
                                // In AngelScript, they must be registered via RegisterObjectBehaviour,
                                // but since our dummy types are asOBJ_POD, they don't even need them to be
                                // registered for compilation to succeed. Passing them to RegisterObjectMethod
                                // will cause asINVALID_DECLARATION and kill the engine.
                                if (child->name == sym->name || (!child->name.empty() && child->name[0] == '~'))
                                {
                                    continue;
                                }
                                engine->RegisterObjectMethod(declName.c_str(), child->BuildSignature().c_str(), asFUNCTION(DummyGeneric), asCALL_GENERIC);
                            }
                            else if (child->kind == SymbolKind::Property || child->kind == SymbolKind::Variable)
                            {
                                engine->RegisterObjectProperty(declName.c_str(), child->BuildSignature().c_str(), 0);
                            }
                        }
                    }
                    else if (sym->kind == SymbolKind::Function)
                    {
                        engine->RegisterGlobalFunction(sym->BuildSignature().c_str(), asFUNCTION(DummyGeneric), asCALL_GENERIC);
                    }
                    else if (sym->kind == SymbolKind::Variable)
                    {
                        static int dummyVar = 0;
                        engine->RegisterGlobalProperty(sym->BuildSignature().c_str(), &dummyVar);
                    }
                }
            }
        };

        // Flatten globals
        std::vector<std::shared_ptr<Symbol>> globals;
        for (const auto &[name, overloads] : table.GetGlobals())
        {
            for (const auto &sym : overloads)
            {
                globals.push_back(sym);
            }
        }

        // Pass 1: Types
        processSymbols(processSymbols, globals, 1);

        // Register Default Array ONLY if it was registered
        if (engine->GetTypeInfoByName((arrayType + "<T>").c_str()) != nullptr ||
            engine->GetTypeInfoByName(arrayType.c_str()) != nullptr)
        {
            engine->RegisterDefaultArrayType((arrayType + "<T>").c_str());
        }

        // Pass 2: Funcdefs
        processSymbols(processSymbols, globals, 2);

        // Pass 3: Methods, properties, functions
        processSymbols(processSymbols, globals, 3);

        engine->ClearMessageCallback();
    }

    bool PredefinedLoader::LoadFromSource(const std::string &source, asIScriptEngine *engine, SymbolTable &table, const std::string &stringType, const std::string &arrayType, std::function<void(const std::string &, int)> logger)
    {
        if (!engine)
            return false;

        // Use a temporary document to parse the source
        Document doc("file:///as.predefined", source);
        SymbolCollector::CollectGlobals(doc, table);

        // Call RegisterSymbols passing the logger pointer
        RegisterSymbols(table, engine, stringType, arrayType, &logger);

        // Generate script code for abstract/shared/mixin classes
        std::string scriptCode;
        auto processAbstracts = [&](auto self, const std::vector<std::shared_ptr<Symbol>> &symbols, const std::string &currentNs) -> void
        {
            for (const auto &sym : symbols)
            {
                if (sym->kind == SymbolKind::Namespace)
                {
                    std::string newNs = currentNs.empty() ? sym->name : currentNs + "::" + sym->name;
                    scriptCode += "namespace " + sym->name + " {\n";
                    self(self, sym->children, newNs);
                    scriptCode += "}\n";
                }
                else if ((sym->kind == SymbolKind::Class && (sym->isAbstract || sym->isShared)) || sym->kind == SymbolKind::Mixin || sym->kind == SymbolKind::Interface)
                {
                    if (sym->kind == SymbolKind::Interface)
                    {
                        if (sym->isShared)
                            scriptCode += "shared ";
                        scriptCode += "interface " + sym->name;
                    }
                    else
                    {
                        if (sym->isShared)
                            scriptCode += "shared ";
                        if (sym->isAbstract)
                            scriptCode += "abstract ";
                        if (sym->kind == SymbolKind::Mixin)
                            scriptCode += "mixin ";
                        scriptCode += "class " + sym->name;
                    }

                    if (!sym->baseClasses.empty())
                    {
                        scriptCode += " : ";
                        for (size_t i = 0; i < sym->baseClasses.size(); ++i)
                        {
                            scriptCode += sym->baseClasses[i];
                            if (i + 1 < sym->baseClasses.size())
                                scriptCode += ", ";
                        }
                    }
                    scriptCode += " {\n";

                    for (const auto &child : sym->children)
                    {
                        if (child->kind == SymbolKind::Method)
                        {
                            scriptCode += "  " + child->BuildSignature() + " {}\n";
                        }
                        else if (child->kind == SymbolKind::Variable)
                        {
                            scriptCode += "  " + child->typeInfo + " " + child->name + ";\n";
                        }
                    }
                    scriptCode += "}\n";
                }
            }
        };

        std::vector<std::shared_ptr<Symbol>> globals;
        for (const auto &kv : table.GetGlobals())
        {
            for (const auto &sym : kv.second)
            {
                globals.push_back(sym);
            }
        }

        processAbstracts(processAbstracts, globals, "");

        // Store it in the engine user data
        std::string *previousCode = static_cast<std::string *>(engine->GetUserData(2000));
        if (previousCode)
            delete previousCode;
        engine->SetUserData(new std::string(scriptCode), 2000);

        return true;
    }

    bool PredefinedLoader::LoadFromFile(const std::string &filePath, asIScriptEngine *engine, SymbolTable &table, const std::string &stringType, const std::string &arrayType, std::function<void(const std::string &, int)> logger)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
            return false;

        std::stringstream buffer;
        buffer << file.rdbuf();

        return LoadFromSource(buffer.str(), engine, table, stringType, arrayType, logger);
    }

    bool PredefinedLoader::FindInWorkspace(const std::string &rootUri, asIScriptEngine *engine, SymbolTable &table, const std::string &stringType, const std::string &arrayType, std::function<void(const std::string &, int)> logger)
    {
        if (rootUri.empty())
            return false;

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
