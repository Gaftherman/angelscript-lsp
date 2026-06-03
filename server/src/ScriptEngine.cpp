/**
 * @file ScriptEngine.cpp
 * @brief Encapsulates memory lifecycle rules and addon interfaces for the script runtime engine.
 */

#include "ScriptEngine.h"
#include <scriptstdstring/scriptstdstring.h>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>
#include <scriptmath/scriptmath.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

ScriptEngine::ScriptEngine() {
    engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
    engine->SetMessageCallback(asFUNCTION(MessageCallback), this, asCALL_CDECL);

    RegisterStdString(engine);
    RegisterScriptArray(engine, true); 
    RegisterStdStringUtils(engine);
    RegisterScriptDictionary(engine);
    RegisterScriptMath(engine);
}

ScriptEngine::~ScriptEngine() {
    if (engine) engine->Release();
}

void ScriptEngine::MessageCallback(const asSMessageInfo *msg, void *param) {
    ScriptEngine* se = static_cast<ScriptEngine*>(param);
    se->AppendDiagnostic({msg->row, msg->col, msg->message, msg->type});
}

int ScriptEngine::IncludeCallback(const char *include, const char *from, CScriptBuilder *builder, void *userParam) {
    fs::path fromPath = fs::path(from);
    fs::path includePath = (fromPath.string() == "script" || fromPath.string() == "") 
        ? fs::path(include) 
        : fromPath.parent_path() / include;

    if (!fs::exists(includePath)) {
        fs::path withExtension = includePath.string() + ".as";
        if (fs::exists(withExtension)) includePath = withExtension;
    }
    std::ifstream file(includePath);
    if (!file.is_open()) return -1; 
    std::stringstream buffer; buffer << file.rdbuf();
    builder->AddSectionFromMemory(includePath.string().c_str(), buffer.str().c_str());
    return 0; 
}

void ScriptEngine::BuildModule(const std::string& moduleName, const std::string& sectionName, const std::string& code) {
    CScriptBuilder builder;
    builder.SetIncludeCallback(IncludeCallback, this);
    builder.StartNewModule(engine, moduleName.c_str());
    builder.AddSectionFromMemory(sectionName.c_str(), code.c_str());
    builder.BuildModule();
}