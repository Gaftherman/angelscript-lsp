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

/**
 * @class ScriptEngine
 * @brief Wrapper class managing the native AngelScript engine instance and compilation diagnostics.
 */
class ScriptEngine
{
public:
    /**
     * @struct EngineDiagnostic
     * @brief Structured diagnostic record for compiler messages emitted during module builds.
     */
    struct EngineDiagnostic
    {
        /** @brief The line number where the diagnostic event occurred. */
        int row;

        /** @brief The column number where the diagnostic event occurred. */
        int col;

        /** @brief The text message describing the compiler output. */
        std::string message;

        /** @brief The severity classification of the diagnostic message (e.g., error, warning, info). */
        int type;
    };

private:
    asIScriptEngine *engine;
    std::vector<EngineDiagnostic> diagnostics;

public:
    /**
     * @brief Default constructor that initializes the native AngelScript engine environment.
     */
    ScriptEngine();

    /**
     * @brief Destructor that cleans up and safely releases the native AngelScript engine instance.
     */
    ~ScriptEngine();

    /**
     * @brief Accessor for the underlying native AngelScript engine instance.
     * @return Pointer to the native asIScriptEngine instance managed by this wrapper.
     */
    asIScriptEngine *GetNativeEngine() const;

    /**
     * @brief Accessor for the collection of diagnostics emitted during module builds.
     * @return A vector of EngineDiagnostic records representing compiler messages.
     */
    const std::vector<EngineDiagnostic> &GetDiagnostics() const;

    /**
     * @brief Clears the current diagnostics collection, preparing for a new module build cycle.
     */
    void ClearDiagnostics();

    /**
     * @brief Appends a new diagnostic record to the diagnostics collection.
     * @param diag The EngineDiagnostic record to be added to the collection.
     */
    void AppendDiagnostic(const EngineDiagnostic &diag);

    /**
     * @brief Triggers an isolated module build routine onto a chosen text slice definition.
     * @param moduleName Targeted compile namespace container identifier.
     * @param sectionName Absolute working filesystem path location corresponding to the source file.
     * @param code In-memory source context code string.
     */
    void BuildModule(const std::string &moduleName, const std::string &sectionName, const std::string &code);

private:
    /**
     * @brief Static callback function for handling compiler messages emitted by the native AngelScript engine.
     * @param msg Pointer to the asSMessageInfo structure containing details about the compiler message.
     * @param param User-defined parameter passed to the callback, expected to be a pointer to the calling ScriptEngine instance.
     */
    static void MessageCallback(const asSMessageInfo *msg, void *param);

    /**
     * @brief Static callback function for handling include directives during module builds.
     * @param include The name of the file being requested for inclusion.
     * @param from The name of the file from which the include directive was encountered.
     * @param builder Pointer to the CScriptBuilder instance managing the current module build process.
     * @param userParam User-defined parameter passed to the callback, expected to be a pointer to the calling ScriptEngine instance.
     * @return An integer status code indicating the success or failure of the include resolution.
     */
    static int IncludeCallback(const char *include, const char *from, CScriptBuilder *builder, void *userParam);
};

#endif // SCRIPT_ENGINE_H