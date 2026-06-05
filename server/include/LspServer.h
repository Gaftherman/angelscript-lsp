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
#include <nlohmann/json.hpp>
#include "ScriptEngine.h"

using json = nlohmann::json;

/**
 * @class AngelScriptLSPServer
 * @brief Core controller class managing Language Server Protocol (LSP) routing, state, and client-server communication.
 */
class AngelScriptLSPServer {
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
    void SendToVSCode(const json& message);

    /**
     * @brief Logs a message to the client output console with a specified log level.
     * @param message The log message content to be sent to the client.
     * @param logType The severity level of the log (1: Error, 2: Warning, 3: Info). Default is 3 (Info).
     */
    void LogRemote(std::string_view message, int logType = 3);

    /**
     * @brief Transforms a given file URI into an absolute filesystem path string.
     * @param uri The file URI to be transformed (e.g., "file:///C:/path/to/file.as").
     * @return A string representing the absolute filesystem path corresponding to the input URI.
     */
    std::string TransformUriToPath(std::string_view uri);

    /**
     * @brief Handles the 'initialize' LSP request by responding with the server's capabilities and supported features.
     * @param id The unique identifier of the LSP request to which this response corresponds.
     */
    void HandleInitialize(json id);

    /**
     * @brief Handles the 'textDocument/didOpen' and 'textDocument/didChange' LSP notifications by caching the document's content and triggering an analysis pass.
     * @param uri The URI of the document that was opened or modified.
     * @param code The full text content of the document.
     */
    void AnalyzeAndReport(const std::string& uri, const std::string& code);

    /**
     * @brief Handles the 'textDocument/semanticTokens/full' LSP request by analyzing the document content and returning semantic token information for syntax highlighting.
     * @param id The unique identifier of the LSP request to which this response corresponds.
     * @param uri The URI of the document for which semantic tokens are being requested.
     */
    void HandleSemanticTokens(json id, const std::string& uri);

    /**
     * @brief Handles the 'textDocument/completion' LSP request by analyzing the document context at the specified position and returning relevant completion items.
     * @param id The unique identifier of the LSP request to which this response corresponds.
     * @param uri The URI of the document for which completion is being requested.
     * @param line The line number (0-based) in the document where the completion request was triggered.
     * @param character The character offset (0-based) in the line where the completion request was triggered.
     */
    void HandleCompletion(json id, const std::string& uri, int line, int character);

    /**
     * @brief Handles the 'textDocument/hover' LSP request by analyzing the document context at the specified position and returning relevant hover information.
     * @param id The unique identifier of the LSP request to which this response corresponds.
     * @param uri The URI of the document for which hover information is being requested.
     * @param line The line number (0-based) in the document where the hover request was triggered.
     * @param character The character offset (0-based) in the line where the hover request was triggered.
     */
    void HandleHover(json id, const std::string& uri, int line, int character);
};

#endif // LSP_SERVER_H