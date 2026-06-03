/**
 * @file ScriptEngine.h
 * @brief Script context compiler orchestration wrapper around native AngelScript engine layouts.
 * @author AngelScript LSP Team
 */

#ifndef SCRIPT_ENGINE_H
#define SCRIPT_ENGINE_H

#include <angelscript.h>
#include <scriptbuilder/scriptbuilder.h>
#include <string>
#include <vector>

class ScriptEngine {
public:
    /**
     * @struct EngineDiagnostic
     * @brief Structured diagnostic record for compiler messages emitted during module builds.
     */
    struct EngineDiagnostic {
        int row;
        int col;
        std::string message;
        int type;
    };

private:
    asIScriptEngine* engine;
    std::vector<EngineDiagnostic> diagnostics;

public:
    ScriptEngine();
    ~ScriptEngine();

    /**
     * @brief Accessor for the underlying native AngelScript engine instance.
     * @return Pointer to the native asIScriptEngine instance managed by this wrapper.
     */
    asIScriptEngine* GetNativeEngine() const { return engine; }

    /**
     * @brief Accessor for the collection of diagnostics emitted during module builds.
     * @return A vector of EngineDiagnostic records representing compiler messages.
     */
    const std::vector<EngineDiagnostic>& GetDiagnostics() const { return diagnostics; }

    /**
     * @brief Clears the current diagnostics collection, preparing for a new module build cycle.
     */
    void ClearDiagnostics() { diagnostics.clear(); }

    /**
     * @brief Appends a new diagnostic record to the diagnostics collection.
     * @param diag The EngineDiagnostic record to be added to the collection.
     */
    void AppendDiagnostic(const EngineDiagnostic& diag) { diagnostics.push_back(diag); }

    /**
     * @brief Triggers an isolated module build routine onto a chosen text slice definition.
     * @param moduleName Targeted compile namespace container identifier.
     * @param sectionName Absolute working filesystem path location.
     * @param code In-memory source context code string.
     */
    void BuildModule(const std::string& moduleName, const std::string& sectionName, const std::string& code);

private:
    /**
     * @brief Static callback function for handling compiler messages emitted by the native AngelScript engine.
     * @param msg Pointer to the asSMessageInfo structure containing details about the compiler message.
     * @param param User-defined parameter passed to the callback, expected to be a pointer to the ScriptEngine instance.
     */
    static void MessageCallback(const asSMessageInfo *msg, void *param);

    /**
     * @brief Static callback function for handling include directives during module builds.
     * @param include The name of the file being included.
     * @param from The name of the file from which the include directive was encountered.
     * @param builder Pointer to the CScriptBuilder instance managing the current module build process.
     * @param userParam User-defined parameter passed to the callback, expected to be a pointer
     */
    static int IncludeCallback(const char *include, const char *from, CScriptBuilder *builder, void *userParam);
};

#endif // SCRIPT_ENGINE_H