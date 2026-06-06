/**
 * @file LspServer.cpp
 * @brief Implements JSON-RPC message parsing and routing workflows compliant with LSP lifecycle rules.
 * @author AngelScript LSP Team
 */

#include "LspServer.h"
#include "TokenHarvester.h"
#include "CompletionHandler.h"

#include <iostream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <fmt/core.h>

#include <fstream>

// CRITICAL FIX: Explicit Win32 binary stream headers to override carriage return text-mode translations
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

/**
 * @brief Launches the main infinite execution loop monitoring client stream traffic blocks.
 */
void AngelScriptLSPServer::Run()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    while (true)
    {
        int contentLength = 0;
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                break;

            std::string_view prefix = "Content-Length: ";
            if (line.compare(0, prefix.length(), prefix) == 0)
            {
                contentLength = std::stoi(std::string(line.substr(prefix.length())));
            }
        }
        if (contentLength == 0)
            continue;

        std::string content(contentLength, ' ');
        std::cin.read(&content[0], contentLength);

        try
        {
            json request = json::parse(content);
            if (request.contains("method"))
            {
                std::string method = request["method"];

                if (method == "initialize")
                    HandleInitialize(request["id"]);
                else if (method == "textDocument/didOpen" || method == "textDocument/didChange")
                {
                    std::string uri = request["params"]["textDocument"]["uri"];
                    std::string text = (method == "textDocument/didOpen")
                                           ? request["params"]["textDocument"]["text"]
                                           : request["params"]["contentChanges"][0]["text"];
                    documentCache[uri] = text;
                    AnalyzeAndReport(uri, text);
                }
                else if (method == "textDocument/semanticTokens/full")
                {
                    std::string uri = request["params"]["textDocument"]["uri"];
                    if (documentCache.find(uri) != documentCache.end())
                        HandleSemanticTokens(request["id"], uri);
                }
                else if (method == "textDocument/completion")
                {
                    json id = request["id"];
                    std::string uri = request["params"]["textDocument"]["uri"];
                    int l = request["params"]["position"]["line"];
                    int c = request["params"]["position"]["character"];
                    if (documentCache.find(uri) != documentCache.end())
                        HandleCompletion(id, uri, l, c);
                }
                else if (method == "textDocument/hover")
                {
                    json id = request["id"];
                    std::string uri = request["params"]["textDocument"]["uri"];
                    int l = request["params"]["position"]["line"];
                    int c = request["params"]["position"]["character"];
                    if (documentCache.find(uri) != documentCache.end())
                        HandleHover(id, uri, l, c);
                }
            }
        }
        catch (...)
        {
        }
    }
}

/**
 * @brief Serializes a JSON message and sends it to the client output stream with proper LSP framing.
 * @param message JSON object representing the LSP response or notification to be sent.
 */
void AngelScriptLSPServer::SendToVSCode(const json &message)
{
    std::string content = message.dump();
    fmt::print("Content-Length: {}\r\n\r\n{}", content.length(), content);
    std::cout << std::flush;
}

/**
 * @brief Logs a message to the client output console with a specified log level.
 * @param message The log message content to be sent to the client.
 * @param logType The severity level of the log (1: Error, 2: Warning, 3: Info).
 */
void AngelScriptLSPServer::LogRemote(std::string_view message, int logType)
{
    json logNotification = {
        {"jsonrpc", "2.0"}, {"method", "window/logMessage"}, {"params", {{"type", logType}, {"message", std::string(message)}}}};
    SendToVSCode(logNotification);
}

/**
 * @brief Transforms a given file URI into an absolute filesystem path string.
 * @param uri The file URI to be transformed (e.g., "file:///C:/path/to/file.as").
 * @return A string representing the absolute filesystem path corresponding to the input URI.
 */
std::string AngelScriptLSPServer::TransformUriToPath(std::string_view uri)
{
    std::string ret(uri);
    if (ret.find("file:///") == 0)
    {
#ifdef _WIN32
        ret = ret.substr(8);
#else
        ret = ret.substr(7);
#endif
    }
    std::string decoded;
    decoded.reserve(ret.length());
    for (size_t i = 0; i < ret.length(); ++i)
    {
        if (ret[i] == '%' && i + 2 < ret.length())
        {
            int hex;
            std::istringstream hexStream(ret.substr(i + 1, 2));
            hexStream >> std::hex >> hex;
            decoded += static_cast<char>(hex);
            i += 2;
        }
        else
            decoded += ret[i];
    }
    return decoded;
}

/**
 * @brief Handles the 'initialize' LSP request by responding with the server's capabilities and supported features.
 * @param id The unique identifier of the LSP request to which this response corresponds.
 */
void AngelScriptLSPServer::HandleInitialize(json id)
{
    json response = {
        {"jsonrpc", "2.0"}, {"id", id}, {"result", {{"capabilities", {{"textDocumentSync", 1}, {"semanticTokensProvider", {{"legend", {{"tokenTypes", std::vector<std::string>{"keyword", "type", "function", "variable", "number", "string", "comment", "operator"}}, {"tokenModifiers", std::vector<std::string>()}}}, {"full", true}}}, {"completionProvider", {{"resolveProvider", false}, {"triggerCharacters", std::vector<std::string>{".", ":", "@"}}}}, {"hoverProvider", true}}}}}};
    SendToVSCode(response);
    LogRemote("AngelScript LSP engine worker module successfully attached.", 3);
}

/**
 * @brief Handles the 'textDocument/didOpen' and 'textDocument/didChange' LSP notifications by caching the document's content and triggering an analysis pass.
 * @param uri The URI of the document that was opened or modified.
 * @param code The full text content of the document.
 */
void AngelScriptLSPServer::AnalyzeAndReport(const std::string &uri, const std::string &code)
{
    scriptEngine.ClearDiagnostics();
    std::string cleanUri = TransformUriToPath(uri);
    scriptEngine.BuildModule("LSPModule", cleanUri, code);

    json diagnosticsArray = json::array();
    for (const auto &diag : scriptEngine.GetDiagnostics())
    {
        int line = std::max(0, diag.row - 1);
        int col = std::max(0, diag.col - 1);
        diagnosticsArray.push_back({{"range", {{"start", {{"line", line}, {"character", col}}}, {"end", {{"line", line}, {"character", col + 5}}}}},
                                    {"severity", (diag.type == asMSGTYPE_WARNING) ? 2 : 1},
                                    {"message", diag.message},
                                    {"source", "AngelScript"}});
    }
    SendToVSCode({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", {{"uri", uri}, {"diagnostics", diagnosticsArray}}}});
}

/**
 * @brief Handles the 'textDocument/semanticTokens/full' LSP request by analyzing the document content and returning semantic token information for syntax highlighting.
 * @param id The unique identifier of the LSP request to which this response corresponds.
 * @param uri The URI of the document for which semantic tokens are being requested.
 */
void AngelScriptLSPServer::HandleSemanticTokens(json id, const std::string &uri)
{
    std::string_view code = documentCache[uri];
    std::vector<int> tokens;
    int prevLine = 0, prevChar = 0, currentLine = 0, currentChar = 0;
    size_t i = 0;

    asIScriptEngine *nativeEng = scriptEngine.GetNativeEngine();
    while (i < code.length())
    {
        asUINT len = 0;
        asETokenClass tc = nativeEng->ParseToken(code.data() + i, static_cast<asUINT>(code.length() - i), &len);
        int tokenType = -1;

        if (tc == asTC_KEYWORD)
            tokenType = 0;
        else if (tc == asTC_VALUE)
            tokenType = (code[i] == '"' || code[i] == '\'') ? 5 : 4;
        else if (tc == asTC_COMMENT)
            tokenType = 6;
        else if (tc == asTC_IDENTIFIER)
        {
            std::string_view text = code.substr(i, len);
            if (text == "string" || text == "array" || text == "dictionary" || text == "int" || text == "float" || text == "bool" || text == "void" || text == "uint")
            {
                tokenType = 1;
            }
            else
            {
                tokenType = 3;
                size_t nextPos = i + len;
                while (nextPos < code.length() && isspace(static_cast<unsigned char>(code[nextPos])))
                    nextPos++;
                if (nextPos < code.length() && code[nextPos] == '(')
                    tokenType = 2;
            }
        }

        if (tokenType != -1)
        {
            int deltaLine = currentLine - prevLine;
            int deltaChar = (deltaLine == 0) ? (currentChar - prevChar) : currentChar;
            tokens.push_back(deltaLine);
            tokens.push_back(deltaChar);
            tokens.push_back(len);
            tokens.push_back(tokenType);
            tokens.push_back(0);
            prevLine = currentLine;
            prevChar = currentChar;
        }

        for (asUINT j = 0; j < len; ++j)
        {
            if (code[i + j] == '\n')
            {
                currentLine++;
                currentChar = 0;
            }
            else
                currentChar++;
        }
        i += (len == 0 ? 1 : len);
    }
    SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"data", tokens}}}});
}

/**
 * @brief Handles the 'textDocument/completion' LSP request by analyzing the document context at the specified position and returning relevant completion items.
 * @param id The unique identifier of the LSP request to which this response corresponds.
 * @param uri The URI of the document for which completion is being requested.
 * @param line The line number (0-based) in the document where the completion request was triggered.
 * @param character The character offset (0-based) in the line where the completion request was triggered.
 */
void AngelScriptLSPServer::HandleCompletion(json id, const std::string &uri, int line, int character)
{
    std::string originalText = documentCache[uri];
    asIScriptEngine *nativeEng = scriptEngine.GetNativeEngine();
    size_t cursorAbsPos = TokenHarvester::GetAbsolutePosition(originalText, line, character);

    TokenHarvester::CompletionContext ctx = TokenHarvester::GetCompletionContext(nativeEng, originalText, cursorAbsPos);

    std::string modifiedText = originalText;
    size_t lineStartPos = 0;
    for (int i = 0; i < line; ++i)
    {
        lineStartPos = originalText.find('\n', lineStartPos);
        if (lineStartPos == std::string::npos)
            break;
        lineStartPos++;
    }
    if (lineStartPos != std::string::npos)
    {
        size_t lineEndPos = originalText.find('\n', lineStartPos);
        if (lineEndPos == std::string::npos)
            lineEndPos = originalText.length();
        for (size_t p = lineStartPos; p < lineEndPos; ++p)
        {
            if (modifiedText[p] != '\r' && modifiedText[p] != '\n')
                modifiedText[p] = ' ';
        }
    }

    std::string cleanUri = TransformUriToPath(uri);
    scriptEngine.BuildModule("LSPModule", cleanUri, modifiedText);
    asIScriptModule *mod = nativeEng->GetModule("LSPModule");

    std::string enclosingClass = "";
    auto customClasses = TokenHarvester::ScanCustomClasses(nativeEng, originalText);
    auto tokenFuncs = TokenHarvester::ScanGlobalFunctions(nativeEng, originalText);
    auto tokenGlobalVars = TokenHarvester::ScanGlobalVariables(nativeEng, originalText);
    auto localVars = TokenHarvester::ScanLocalVariables(nativeEng, originalText, cursorAbsPos, enclosingClass, customClasses, tokenGlobalVars, tokenFuncs);

    std::ofstream("angelscript_ast_debug.txt", std::ios_base::trunc).close();

    auto logger = [](const std::string &msg)
    {
        std::cerr << "[AST Debug] " << msg << std::endl;
    };

    CompletionHandler handler(nativeEng, mod, ctx, enclosingClass, localVars, customClasses, tokenFuncs, tokenGlobalVars, logger);
    json itemsArray = handler.GenerateItems(originalText, cursorAbsPos);

    json response = {
        {"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isIncomplete", false}, {"items", itemsArray}}}};
    SendToVSCode(response);
}

/**
 * @brief Handles the 'textDocument/hover' LSP request by analyzing the document context at the specified position and returning relevant hover information.
 * @param id The unique identifier of the LSP request to which this response corresponds.
 * @param uri The URI of the document for which hover information is being requested.
 * @param line The line number (0-based) in the document where the hover request was triggered.
 * @param character The character offset (0-based) in the line where the hover request was triggered.
 */
void AngelScriptLSPServer::HandleHover(json id, const std::string &uri, int line, int character)
{
    std::string_view text = documentCache[uri];
    asIScriptEngine *nativeEng = scriptEngine.GetNativeEngine();

    std::string codeCopy(text);
    std::istringstream stream(codeCopy);
    std::string currentLineText;
    for (int i = 0; i <= line; ++i)
    {
        if (!std::getline(stream, currentLineText))
            break;
    }

    int start = character;
    if (start > (int)currentLineText.length())
        start = (int)currentLineText.length();
    int end = start;

    while (start > 0 && (isalnum(static_cast<unsigned char>(currentLineText[start - 1])) || currentLineText[start - 1] == '_'))
        start--;
    while (end < (int)currentLineText.length() && (isalnum(static_cast<unsigned char>(currentLineText[end])) || currentLineText[end] == '_'))
        end++;

    std::string word = currentLineText.substr(start, end - start);
    if (word.empty())
    {
        SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}});
        return;
    }

    size_t cursorAbsPos = TokenHarvester::GetAbsolutePosition(text, line, character);

    auto customClasses = TokenHarvester::ScanCustomClasses(nativeEng, text);
    auto tokenFuncs = TokenHarvester::ScanGlobalFunctions(nativeEng, text);
    auto tokenGlobalVars = TokenHarvester::ScanGlobalVariables(nativeEng, text);

    std::string enclosingClass = "";
    auto localVars = TokenHarvester::ScanLocalVariables(nativeEng, text, cursorAbsPos, enclosingClass, customClasses, tokenGlobalVars, tokenFuncs);

    std::string hoverResult = "";

    for (const auto &v : localVars)
    {
        if (v.name == word)
        {
            hoverResult = fmt::format("```cpp\n(local) {} {}\n```", v.typeName, v.name);
            break;
        }
    }

    if (hoverResult.empty())
    {
        for (const auto &v : tokenGlobalVars)
        {
            if (v.name == word)
            {
                hoverResult = fmt::format("```cpp\n(global) {} {}\n```", v.typeName, v.name);
                break;
            }
        }
    }

    if (hoverResult.empty())
    {
        for (const auto &c : customClasses)
        {
            if (c.name == word)
            {
                hoverResult = fmt::format("```cpp\nclass {}\n```\n*User-defined script type.*", c.name);
                break;
            }
        }
    }

    if (hoverResult.empty())
    {
        for (const auto &f : tokenFuncs)
        {
            if (f.name == word)
            {
                hoverResult = fmt::format("```cpp\n(function) {}\n```", f.declaration);
                break;
            }
        }
    }

    if (hoverResult.empty())
    {
        asITypeInfo *typeInfo = nativeEng->GetTypeInfoByName(word.c_str());
        if (typeInfo)
        {
            hoverResult = fmt::format("```cpp\nclass {}\n```\n*Native C++ object.*", typeInfo->GetName());
        }
    }

    if (hoverResult.empty())
    {
        for (asUINT f = 0; f < nativeEng->GetGlobalFunctionCount(); f++)
        {
            asIScriptFunction *func = nativeEng->GetGlobalFunctionByIndex(f);
            if (std::string(func->GetName()) == word)
            {
                hoverResult = fmt::format("```cpp\n(native function) {}\n```", func->GetDeclaration(false, false, true));
                break;
            }
        }
    }

    if (!hoverResult.empty())
    {
        json response = {
            {"jsonrpc", "2.0"}, {"id", id}, {"result", {{"contents", {{"kind", "markdown"}, {"value", hoverResult}}}}}};
        SendToVSCode(response);
    }
    else
    {
        SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}});
    }
}