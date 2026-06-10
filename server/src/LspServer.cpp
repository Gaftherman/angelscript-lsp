/**
 * @file LspServer.cpp
 * @brief Implements JSON-RPC message parsing and routing workflows compliant with LSP lifecycle rules.
 * @author AngelScript LSP Team
 */

#include "LspServer.h"
#include "TokenHarvester.h"
#include "CompletionHandler.h"
#include "HoverHandler.h"
#include "SafeCtype.h"

#include <iostream>
#include <sstream>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <unordered_set>
#include <unordered_map>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace LspServer
{
    /**
     * @brief Blanks out the content of a specific line to prevent parser syntax errors.
     */
    std::string BlankOutLine(const std::string &text, int line)
    {
        std::string modifiedText = text;
        size_t lineStartPos = 0;
        size_t lineEndPos = 0;

        for (int i = 0; i < line; ++i)
        {
            lineStartPos = text.find('\n', lineStartPos);

            if (lineStartPos == std::string::npos)
            {
                return modifiedText;
            }

            lineStartPos++;
        }

        lineEndPos = text.find('\n', lineStartPos);

        if (lineEndPos == std::string::npos)
        {
            lineEndPos = text.length();
        }

        for (size_t p = lineStartPos; p < lineEndPos; ++p)
        {
            if (modifiedText[p] != '\r' && modifiedText[p] != '\n')
            {
                modifiedText[p] = ' ';
            }
        }

        return modifiedText;
    }

    /**
     * @brief Extracts the full alphanumeric word under the specified cursor coordinate.
     */
    std::string ExtractWordAtPosition(std::string_view text, int line, int character)
    {
        std::string codeCopy(text);
        std::istringstream stream(codeCopy);
        std::string currentLineText;
        int start = character;
        int end = 0;

        for (int i = 0; i <= line; ++i)
        {
            if (!std::getline(stream, currentLineText))
            {
                break;
            }
        }

        if (start > (int)currentLineText.length())
        {
            start = (int)currentLineText.length();
        }

        end = start;

        while (start > 0 && (SAFE_IS_ALNUM(static_cast<unsigned char>(currentLineText[start - 1])) || currentLineText[start - 1] == '_'))
        {
            start--;
        }

        while (end < (int)currentLineText.length() && (SAFE_IS_ALNUM(static_cast<unsigned char>(currentLineText[end])) || currentLineText[end] == '_'))
        {
            end++;
        }

        return currentLineText.substr(start, end - start);
    }

    /**
     * @brief Collects and populates script classes and object structures compiled inside a module.
     */
    void PopulateCustomClassesFromModule(asIScriptEngine *engine, asIScriptModule *mod, std::vector<TokenHarvester::ScriptClass> &customClasses)
    {
        if (!mod || !engine)
        {
            return;
        }

        for (asUINT t = 0; t < mod->GetObjectTypeCount(); t++)
        {
            asITypeInfo *typeInfo = mod->GetObjectTypeByIndex(t);

            if (!typeInfo)
            {
                continue;
            }

            std::string className = typeInfo->GetName();
            auto it = std::find_if(customClasses.begin(), customClasses.end(),
                                   [&](const TokenHarvester::ScriptClass &c)
                                   { return c.name == className; });

            if (it == customClasses.end())
            {
                TokenHarvester::ScriptClass extClass;
                extClass.name = className;

                for (asUINT p = 0; p < typeInfo->GetPropertyCount(); p++)
                {
                    const char *pName = nullptr;
                    int pTypeId = 0;
                    typeInfo->GetProperty(p, &pName, &pTypeId);
                    const char *pDecl = engine->GetTypeDeclaration(pTypeId, true);

                    if (pName && pDecl)
                    {
                        extClass.properties.push_back({pName, pDecl, "public"});
                    }
                }

                for (asUINT m = 0; m < typeInfo->GetMethodCount(); m++)
                {
                    asIScriptFunction *method = typeInfo->GetMethodByIndex(m);

                    if (method)
                    {
                        const char *rDecl = engine->GetTypeDeclaration(method->GetReturnTypeId(), true);
                        bool isConstructorOrDestructor = (method->GetName() == className || method->GetName() == ("~" + className));

                        extClass.methods.push_back({method->GetName(),
                                                    rDecl ? rDecl : "void",
                                                    method->GetDeclaration(true, false, true),
                                                    "public",
                                                    isConstructorOrDestructor});
                    }
                }

                customClasses.push_back(extClass);
            }
        }

        for (asUINT e = 0; e < mod->GetEnumCount(); e++)
        {
            asITypeInfo *enumType = mod->GetEnumByIndex(e);

            if (!enumType)
            {
                continue;
            }

            std::string enumName = enumType->GetName();
            auto it = std::find_if(customClasses.begin(), customClasses.end(),
                                   [&](const TokenHarvester::ScriptClass &c)
                                   { return c.name == enumName; });

            if (it != customClasses.end())
            {
                it->properties.clear();
                it->methods.clear();

                for (asUINT v = 0; v < enumType->GetEnumValueCount(); v++)
                {
                    asINT64 val = 0;
                    const char *enumValName = enumType->GetEnumValueByIndex(v, &val);

                    if (enumValName)
                    {
                        it->properties.push_back({enumValName, enumName, "public"});
                    }
                }
            }
            else
            {
                TokenHarvester::ScriptClass extEnum;
                extEnum.name = enumName;

                for (asUINT v = 0; v < enumType->GetEnumValueCount(); v++)
                {
                    asINT64 val = 0;
                    const char *enumValName = enumType->GetEnumValueByIndex(v, &val);

                    if (enumValName)
                    {
                        extEnum.properties.push_back({enumValName, enumName, "public"});
                    }
                }

                customClasses.push_back(extEnum);
            }
        }
    }

    /**
     * @brief Looks up custom object types and enums declared within the current active module scope.
     */
    bool IsCustomScriptType(asIScriptModule *mod, const std::string &name)
    {
        if (!mod)
        {
            return false;
        }

        for (asUINT t = 0; t < mod->GetObjectTypeCount(); t++)
        {
            if (asITypeInfo *ti = mod->GetObjectTypeByIndex(t))
            {
                if (ti->GetName() == name)
                {
                    return true;
                }
            }
        }

        for (asUINT e = 0; e < mod->GetEnumCount(); e++)
        {
            if (asITypeInfo *ti = mod->GetEnumByIndex(e))
            {
                if (ti->GetName() == name)
                {
                    return true;
                }
            }
        }

        return false;
    }

    /**
     * @brief Performs comprehensive introspection to verify if an identifier is a registered structural data type.
     */
    bool IsEngineOrScriptType(asIScriptEngine *engine, asIScriptModule *mod, const std::string &name)
    {
        if (engine && engine->GetTypeIdByDecl(name.c_str()) >= 0)
        {
            return true;
        }

        return IsCustomScriptType(mod, name);
    }
}

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
        std::string content = "";

        while (std::getline(std::cin, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            if (line.empty())
            {
                break;
            }

            std::string_view prefix = "Content-Length: ";

            if (line.compare(0, prefix.length(), prefix) == 0)
            {
                contentLength = std::stoi(std::string(line.substr(prefix.length())));
            }
        }

        if (contentLength == 0)
        {
            continue;
        }

        content.assign(contentLength, ' ');
        std::cin.read(&content[0], contentLength);

        try
        {
            json request = json::parse(content);

            if (request.contains("method"))
            {
                std::string method = request["method"];

                if (method == "initialize")
                {
                    HandleInitialize(request["id"]);
                }
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
                    {
                        HandleSemanticTokens(request["id"], uri);
                    }
                }
                else if (method == "textDocument/completion")
                {
                    json id = request["id"];
                    std::string uri = request["params"]["textDocument"]["uri"];
                    int l = request["params"]["position"]["line"];
                    int c = request["params"]["position"]["character"];

                    if (documentCache.find(uri) != documentCache.end())
                    {
                        HandleCompletion(id, uri, l, c);
                    }
                }
                else if (method == "textDocument/hover")
                {
                    json id = request["id"];
                    std::string uri = request["params"]["textDocument"]["uri"];
                    int l = request["params"]["position"]["line"];
                    int c = request["params"]["position"]["character"];

                    if (documentCache.find(uri) != documentCache.end())
                    {
                        HandleHover(id, uri, l, c);
                    }
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
 */
void AngelScriptLSPServer::SendToVSCode(const json &message)
{
    std::string content = message.dump();

    fmt::print("Content-Length: {}\r\n\r\n{}", content.length(), content);
    std::cout << std::flush;
}

/**
 * @brief Logs a message to the client output console with a specified log level.
 */
void AngelScriptLSPServer::LogRemote(std::string_view message, int logType)
{
    json logNotification = {
        {"jsonrpc", "2.0"}, {"method", "window/logMessage"}, {"params", {{"type", logType}, {"message", std::string(message)}}}};

    SendToVSCode(logNotification);
}

/**
 * @brief Transforms a given file URI into an absolute filesystem path string.
 */
std::string AngelScriptLSPServer::TransformUriToPath(std::string_view uri)
{
    std::string ret(uri);
    std::string decoded;

    if (ret.find("file:///") == 0)
    {
#ifdef _WIN32
        ret = ret.substr(8);
#else
        ret = ret.substr(7);
#endif
    }

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
        {
            decoded += ret[i];
        }
    }

    return decoded;
}

/**
 * @brief Handles the 'initialize' LSP request by responding with the server's capabilities.
 */
void AngelScriptLSPServer::HandleInitialize(json id)
{
    json response = {
        {"jsonrpc", "2.0"}, {"id", id}, {"result", {{"capabilities", {{"textDocumentSync", 1}, {"semanticTokensProvider", {{"legend", {{"tokenTypes", std::vector<std::string>{"keyword", "type", "function", "variable", "number", "string", "comment", "operator"}}, {"tokenModifiers", std::vector<std::string>()}}}, {"full", true}}}, {"completionProvider", {{"resolveProvider", false}, {"triggerCharacters", std::vector<std::string>{".", ":", "@"}}}}, {"hoverProvider", true}}}}}};

    SendToVSCode(response);
    LogRemote("AngelScript LSP engine worker module successfully attached.", 3);
}

/**
 * @brief Compiles code block buffers and reports structural diagnostic compilation messages.
 */
void AngelScriptLSPServer::AnalyzeAndReport(const std::string &uri, const std::string &code)
{
    std::string cleanUri = TransformUriToPath(uri);
    json diagnosticsArray = json::array();
    fs::path currentPath(cleanUri);
    fs::path baseDir = currentPath.parent_path();
    std::istringstream stream(code);
    std::string lineStr;
    int lineIndex = 0;

    scriptEngine.ClearDiagnostics();
    scriptEngine.BuildModule("LSPModule", cleanUri, code);

    for (const auto &diag : scriptEngine.GetDiagnostics())
    {
        int line = std::max(0, diag.row - 1);
        int col = std::max(0, diag.col - 1);

        diagnosticsArray.push_back({{"range", {{"start", {{"line", line}, {"character", col}}}, {"end", {{"line", line}, {"character", col + 5}}}}},
                                    {"severity", (diag.type == asMSGTYPE_WARNING) ? 2 : 1},
                                    {"message", diag.message},
                                    {"source", "AngelScript"}});
    }

    while (std::getline(stream, lineStr))
    {
        size_t firstChar = lineStr.find_first_not_of(" \t\r\n");

        if (firstChar != std::string::npos && lineStr[firstChar] == '#')
        {
            size_t includePos = lineStr.find("include", firstChar + 1);

            if (includePos != std::string::npos)
            {
                bool validIncludeKeyword = true;

                for (size_t check = firstChar + 1; check < includePos; ++check)
                {
                    if (lineStr[check] != ' ' && lineStr[check] != '\t')
                    {
                        validIncludeKeyword = false;
                        break;
                    }
                }

                if (validIncludeKeyword)
                {
                    size_t startDelim = lineStr.find_first_of("\"<", includePos + 7);

                    if (startDelim != std::string::npos)
                    {
                        char closeChar = (lineStr[startDelim] == '"') ? '"' : '>';
                        size_t endDelim = lineStr.find(closeChar, startDelim + 1);

                        if (endDelim != std::string::npos)
                        {
                            std::string includeFileName = lineStr.substr(startDelim + 1, endDelim - startDelim - 1);
                            fs::path absolutePath = baseDir / fs::path(includeFileName);

                            if (!fs::exists(absolutePath))
                            {
                                diagnosticsArray.push_back({{"range", {{"start", {{"line", lineIndex}, {"character", 0}}}, {"end", {{"line", lineIndex}, {"character", static_cast<int>(lineStr.length())}}}}},
                                                            {"severity", 1},
                                                            {"message", fmt::format("Preprocessor Error: Include file '{}' not found on path.", includeFileName)},
                                                            {"source", "AngelScript LSP Preprocessor"}});
                            }
                        }
                    }
                }
            }
        }

        lineIndex++;
    }

    SendToVSCode({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", {{"uri", uri}, {"diagnostics", diagnosticsArray}}}});
}

/**
 * @brief Computes token streams and responds with metadata layouts mapping syntax highlight profiles.
 */
void AngelScriptLSPServer::HandleSemanticTokens(json id, const std::string &uri)
{
    std::string_view code = documentCache[uri];
    std::vector<int> tokens;
    int prevLine = 0;
    int prevChar = 0;
    int currentLine = 0;
    int currentChar = 0;
    size_t i = 0;

    asIScriptEngine *nativeEng = scriptEngine.GetNativeEngine();
    asIScriptModule *mod = nativeEng->GetModule("LSPModule");

    while (i < code.length())
    {
        asUINT len = 0;
        asETokenClass tc = nativeEng->ParseToken(code.data() + i, static_cast<asUINT>(code.length() - i), &len);
        int tokenType = -1;

        if (tc == asTC_KEYWORD)
        {
            tokenType = 0;
        }
        else if (tc == asTC_VALUE)
        {
            tokenType = (code[i] == '"' || code[i] == '\'') ? 5 : 4;
        }
        else if (tc == asTC_COMMENT)
        {
            tokenType = 6;
        }
        else if (tc == asTC_IDENTIFIER)
        {
            std::string_view text = code.substr(i, len);
            std::string textStr(text);

            if (LspServer::IsEngineOrScriptType(nativeEng, mod, textStr))
            {
                tokenType = 1;
            }
            else
            {
                tokenType = 3;
                size_t nextPos = i + len;

                while (nextPos < code.length() && isspace(static_cast<unsigned char>(code[nextPos])))
                {
                    nextPos++;
                }

                if (nextPos < code.length() && code[nextPos] == '(')
                {
                    tokenType = 2;
                }
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
            {
                currentChar++;
            }
        }

        i += (len == 0 ? 1 : len);
    }

    SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"data", tokens}}}});
}

/**
 * @brief Processes completion context trees and calculates in-scope structural data lists.
 */
void AngelScriptLSPServer::HandleCompletion(json id, const std::string &uri, int line, int character)
{
    std::string originalText = documentCache[uri];
    asIScriptEngine *nativeEng = scriptEngine.GetNativeEngine();
    size_t cursorAbsPos = TokenHarvester::GetAbsolutePosition(originalText, line, character);
    TokenHarvester::CompletionContext ctx = TokenHarvester::GetCompletionContext(nativeEng, originalText, cursorAbsPos);
    std::string modifiedText = LspServer::BlankOutLine(originalText, line);
    std::string cleanUri = TransformUriToPath(uri);
    asIScriptModule *mod = nullptr;
    std::string enclosingClass = "";
    std::vector<TokenHarvester::ScriptClass> customClasses;
    std::vector<TokenHarvester::GlobalFunction> tokenFuncs;
    std::vector<TokenHarvester::GlobalVariable> tokenGlobalVars;
    std::vector<TokenHarvester::LocalVariable> localVars;

    auto logger = [](const std::string &msg)
    {
        std::cerr << "[AST Debug] " << msg << std::endl;
    };

    scriptEngine.BuildModule("LSPModule", cleanUri, modifiedText);
    mod = nativeEng->GetModule("LSPModule");
    customClasses = TokenHarvester::ScanCustomClasses(nativeEng, originalText);

    LspServer::PopulateCustomClassesFromModule(nativeEng, mod, customClasses);

    tokenFuncs = TokenHarvester::ScanGlobalFunctions(nativeEng, originalText);
    tokenGlobalVars = TokenHarvester::ScanGlobalVariables(nativeEng, originalText);
    localVars = TokenHarvester::ScanLocalVariables(nativeEng, originalText, cursorAbsPos, enclosingClass, customClasses, tokenGlobalVars, tokenFuncs);

    std::ofstream("angelscript_ast_debug.txt", std::ios_base::trunc).close();

    CompletionHandler handler(nativeEng, mod, ctx, enclosingClass, localVars, customClasses, tokenFuncs, tokenGlobalVars, logger);
    json itemsArray = handler.GenerateItems(originalText, cursorAbsPos);
    json response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isIncomplete", false}, {"items", itemsArray}}}};

    SendToVSCode(response);
}

/**
 * @brief Active syntactic scope interrogation handler matching AngelScript's official EBNF grammar rules.
 * Resolves local types, properties, funcdefs, lambdas, and native method signatures for LSP Hover notifications.
 */
void AngelScriptLSPServer::HandleHover(json id, const std::string &uri, int line, int character)
{
    std::string originalText = documentCache[uri];
    asIScriptEngine *nativeEng = scriptEngine.GetNativeEngine();

    HoverHandler handler(nativeEng, originalText, line, character);
    json hoverResult = handler.Process();

    json response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", hoverResult}};
    SendToVSCode(response);
}