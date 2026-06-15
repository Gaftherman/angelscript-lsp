/**
 * @file LspServer.h
 * @brief Core Language Server Protocol routing interface controller layout.
 * @author AngelScript LSP Team
 */

#ifndef LSP_SERVER_H
#define LSP_SERVER_H

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "ScriptEngine.h"
#include "TokenHarvester.h"

using json = nlohmann::json;

/**
 * @namespace LspServer
 * @brief Contains auxiliary utility scanners, string manipulation helpers, and data-type validation
 * predicates optimized for Language Server Protocol workflows.
 */
namespace LspServer
{
    /**
     * @brief Overwrites the contents of a targeted script line with whitespaces to preserve line numbers
     * while isolating parsing segments.
     * @param text Complete original source text string reference.
     * @param line Target line index (0-based) to be blanked out.
     * @return A modified string copy where the specified line contents have been replaced with whitespaces.
     */
    std::string BlankOutLine(const std::string &text, int line);

    /**
     * @brief Extracts the full alphanumeric word under the specified cursor coordinate using zero-allocation
     * string segmentation.
     * @param text String view window pointing directly to the multi-line source buffer content.
     * @param line Target coordinate row layout index (0-based).
     * @param character Target coordinate horizontal column offset index (0-based).
     * @return A string view capturing only the isolated word sequence boundary characters.
     */
    std::string_view ExtractWordAtPosition(std::string_view text, int line, int character);

    /**
     * @brief Collects and populates script classes and object structures compiled inside an active module
     * utilizing an accelerated cache mapping profile.
     * @param engine Pointer to the active core AngelScript compiler engine instance framework.
     * @param mod Pointer to the target active script compilation unit storage segment module.
     * @param customClasses Extracted metadata associative lookup structure profile collection to populate.
     */
    void PopulateCustomClassesFromModule(asIScriptEngine *engine, asIScriptModule *mod, std::vector<TokenHarvester::ScriptClass> &customClasses);

    /**
     * @brief Looks up custom object types and enums declared within the current active compilation module scope.
     * @param mod Pointer to the active script compilation module instance.
     * @param name Name identifier string of the candidate type layout to verify.
     * @return True if the symbol profile explicitly matches a custom object type or enum statement descriptor, false otherwise.
     */
    bool IsCustomScriptType(asIScriptModule *mod, const std::string &name);

    /**
     * @brief Performs comprehensive introspection to verify if an identifier sequence is a registered structural data type.
     * @param engine Pointer to the active core AngelScript compiler engine instance framework.
     * @param mod Pointer to the active script compilation unit storage segment module.
     * @param name Candidate token keyword name string profile to evaluate.
     * @return True if validation confirms native registration or custom translation symbol mapping, false otherwise.
     */
    bool IsEngineOrScriptType(asIScriptEngine *engine, asIScriptModule *mod, const std::string &name);

    /**
     * @brief Evaluation predicate determining if an incoming method matches LSP document lifecycle state notifications.
     * @param method The method identifier string view token extracted from the incoming JSON-RPC payload framing.
     * @return True if notification string matches standard document lifecycle events (`didOpen`, `didChange`), false otherwise.
     */
    bool IsLspNotification(std::string_view method) noexcept;

    /**
     * @brief Scans a source line character range to isolate and extract path boundaries of preprocessor include statements.
     * @param line Raw single-line script character sequence view slice to analyze.
     * @param outPathStart Reference receiving the precise beginning index offset of the targeted file location substring path.
     * @param outPathEnd Reference receiving the terminal boundary ending index offset of the targeted file location substring path.
     * @return True if a correctly formed preprocessor include sequence pattern boundary was resolved, false otherwise.
     */
    bool IsIncludeDirective(std::string_view line, size_t &outPathStart, size_t &outPathEnd) noexcept;
}

/**
 * @class AngelScriptLSPServer
 * @brief Core controller class managing Language Server Protocol (LSP) routing, state, and client-server communication.
 */
class AngelScriptLSPServer
{
private:
    ScriptEngine scriptEngine;
    std::unordered_map<std::string, std::string> documentCache;

public:
    /**
     * @brief Default constructor for the AngelScriptLSPServer.
     */
    AngelScriptLSPServer() = default;

    /**
     * @brief Default destructor for the AngelScriptLSPServer.
     */
    ~AngelScriptLSPServer() = default;

    /**
     * @brief Launches the main infinite execution loop monitoring client stream traffic blocks.
     */
    void Run();

private:
    /**
     * @brief Serializes a JSON message and sends it to the client output stream with proper LSP framing.
     * @param message JSON object representing the LSP response or notification to be sent.
     */
    void SendToVSCode(const json &message);

    /**
     * @brief Logs a message to the client output console with a specified log level.
     * @param message The log message content to be sent to the client.
     * @param logType The severity level of the log (1: Error, 2: Warning, 3: Info).
     */
    void LogRemote(std::string_view message, int logType = 3);

    /**
     * @brief Transforms a given file URI into an absolute filesystem path string.
     * @param uri The file URI to be transformed.
     * @return A string representing the absolute filesystem path corresponding to the input URI.
     */
    std::string TransformUriToPath(std::string_view uri);

    /**
     * @brief Handles the 'initialize' LSP request by responding with the server's capabilities and supported features.
     * @param id The unique identifier of the LSP request to which this response corresponds.
     */
    void HandleInitialize(json id);

    /**
     * @brief Handles the 'textDocument/didOpen' and 'textDocument/didChange' LSP notifications.
     * @param uri The URI of the document that was opened or modified.
     * @param code The full text content of the document.
     */
    void AnalyzeAndReport(const std::string &uri, const std::string &code);

    /**
     * @brief Handles the 'textDocument/semanticTokens/full' LSP request.
     * @param id The unique identifier of the LSP request to which this response corresponds.
     * @param uri The URI of the document for which semantic tokens are being requested.
     */
    void HandleSemanticTokens(json id, const std::string &uri);

    /**
     * @brief Handles the 'textDocument/completion' LSP request.
     * @param id The unique identifier of the LSP request to which this response corresponds.
     * @param uri The URI of the document for which completion is being requested.
     * @param line The line number (0-based) in the document where the completion request was triggered.
     * @param character The character offset (0-based) in the line where the completion request was triggered.
     */
    void HandleCompletion(json id, const std::string &uri, int line, int character);

    /**
     * @brief Handles the 'textDocument/hover' LSP request.
     * @param id The unique identifier of the LSP request to which this response corresponds.
     * @param uri The URI of the document for which hover information is being requested.
     * @param line The line number (0-based) in the document where the hover request was triggered.
     * @param character The character offset (0-based) in the line where the hover request was triggered.
     */
    void HandleHover(json id, const std::string &uri, int line, int character);
};

#endif // LSP_SERVER_H