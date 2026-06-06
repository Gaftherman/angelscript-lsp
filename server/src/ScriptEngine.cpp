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

/**
 * @brief Constructs the ScriptEngine and initializes the core AngelScript environment.
 * * Instantiates the native AngelScript engine, binds the diagnostic message callback,
 * and registers essential standard library add-ons including strings, arrays,
 * dictionaries, and math functions.
 */
ScriptEngine::ScriptEngine()
{
    engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
    engine->SetMessageCallback(asFUNCTION(MessageCallback), this, asCALL_CDECL);

    RegisterStdString(engine);
    RegisterScriptArray(engine, true);
    RegisterStdStringUtils(engine);
    RegisterScriptDictionary(engine);
    RegisterScriptMath(engine);
}

/**
 * @brief Destructs the ScriptEngine, ensuring safe memory teardown.
 * * Explicitly releases the underlying native AngelScript engine instance if it exists,
 * preventing memory leaks upon termination.
 */
ScriptEngine::~ScriptEngine()
{
    if (engine)
        engine->Release();
}

/**
 * @brief Internal callback triggered by the AngelScript engine for compiler messages.
 * * Maps native diagnostic messages (errors, warnings, info) into the internal
 * diagnostic tracking collection for LSP reporting.
 * * @param msg Pointer to the structure containing diagnostic message details.
 * @param param Opaque user pointer context, expected to be the active ScriptEngine instance.
 */
void ScriptEngine::MessageCallback(const asSMessageInfo *msg, void *param)
{
    ScriptEngine *se = static_cast<ScriptEngine *>(param);
    se->AppendDiagnostic({msg->row, msg->col, msg->message, msg->type});
}

/**
 * @brief Internal callback invoked by the script builder to resolve and load included script files.
 * * Processes `#include` directives by resolving file paths relative to the current working
 * directory or the origin file, appending `.as` extensions if necessary, and loading the
 * source content into the builder's memory space.
 * * @param include The raw string name or path of the requested include file.
 * @param from The path of the file that triggered the include directive.
 * @param builder Pointer to the script builder coordinating the current module compilation.
 * @param userParam Opaque user pointer context, conventionally pointing to the ScriptEngine.
 * @return An integer status code: 0 upon successful read and inclusion, or -1 if the file could not be opened.
 */
int ScriptEngine::IncludeCallback(const char *include, const char *from, CScriptBuilder *builder, void *userParam)
{
    fs::path fromPath = fs::path(from);
    fs::path includePath = (fromPath.string() == "script" || fromPath.string() == "")
                               ? fs::path(include)
                               : fromPath.parent_path() / include;

    if (!fs::exists(includePath))
    {
        fs::path withExtension = includePath.string() + ".as";
        if (fs::exists(withExtension))
            includePath = withExtension;
    }
    std::ifstream file(includePath);
    if (!file.is_open())
        return -1;
    std::stringstream buffer;
    buffer << file.rdbuf();
    builder->AddSectionFromMemory(includePath.string().c_str(), buffer.str().c_str());
    return 0;
}

/**
 * @brief Orchestrates the compilation of a fresh script module from memory context.
 * * Leverages the script builder utility to define a new module boundary, injects the
 * provided source text section, and executes the build process, populating the internal
 * diagnostics cache in the process.
 * * @param moduleName The designated namespace identifier for the compiled module.
 * @param sectionName The nominal tag or filesystem path serving as the origin of the code block.
 * @param code The raw textual source string to be evaluated and compiled.
 */
void ScriptEngine::BuildModule(const std::string &moduleName, const std::string &sectionName, const std::string &code)
{
    CScriptBuilder builder;
    builder.SetIncludeCallback(IncludeCallback, this);
    builder.StartNewModule(engine, moduleName.c_str());
    builder.AddSectionFromMemory(sectionName.c_str(), code.c_str());
    builder.BuildModule();
}