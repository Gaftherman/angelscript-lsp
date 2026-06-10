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
#include <algorithm>
#include <optional>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace LspConstants
{
    constexpr std::string_view MethodInitialize = "initialize";
    constexpr std::string_view MethodDidOpen = "textDocument/didOpen";
    constexpr std::string_view MethodDidChange = "textDocument/didChange";
    constexpr std::string_view MethodSemanticTokens = "textDocument/semanticTokens/full";
    constexpr std::string_view MethodCompletion = "textDocument/completion";
    constexpr std::string_view MethodHover = "textDocument/hover";

    constexpr std::string_view UriFilePrefixWin = "file:///";
    constexpr std::string_view UriFilePrefixUnix = "file://";
    constexpr std::string_view ContentLengthPrefix = "Content-Length: ";
    constexpr std::string_view IncludeKeyword = "include";
    constexpr std::string_view DiagnosticSource = "AngelScript";
    constexpr std::string_view PreprocessorSource = "AngelScript LSP Preprocessor";
}

namespace LspServer
{
    /**
     * @brief Overwrites the contents of a targeted script line with whitespaces to preserve line numbers while isolating parsing segments.
     * @param text Complete original source text string.
     * @param line Target line index (0-based) to overwrite.
     * @return Modified string with the targeted line blanked out.
     */
    std::string BlankOutLine(const std::string &text, int line)
    {
        std::string modifiedText = text;
        size_t lineStartPos = 0;
        size_t lineEndPos = 0;
        size_t textLen = text.length();
        int i = 0;
        size_t p = 0;

        for (i = 0; i < line; ++i)
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
            lineEndPos = textLen;
        }

        for (p = lineStartPos; p < lineEndPos; ++p)
        {
            if (modifiedText[p] != '\r' && modifiedText[p] != '\n')
            {
                modifiedText[p] = ' ';
            }
        }

        return modifiedText;
    }

    /**
     * @brief Extracts the full alphanumeric word under the specified cursor coordinate using zero-allocation scanning techniques.
     * @param text String view reference pointing directly to the loaded file buffer content.
     * @param line Target coordinate row layout index (0-based).
     * @param character Target coordinate horizontal column offset index (0-based).
     * @return View block capturing only the selected token characters sequence.
     */
    std::string_view ExtractWordAtPosition(std::string_view text, int line, int character)
    {
        size_t lineStartPos = 0;
        size_t lineEndPos = 0;
        size_t textLen = text.length();
        int i = 0;
        std::string_view lineText = "";
        int start = character;
        int end = 0;
        unsigned char startChar = 0;
        unsigned char endChar = 0;

        for (i = 0; i < line; ++i)
        {
            lineStartPos = text.find('\n', lineStartPos);
            if (lineStartPos == std::string::npos)
            {
                return "";
            }
            lineStartPos++;
        }

        lineEndPos = text.find('\n', lineStartPos);
        if (lineEndPos == std::string::npos)
        {
            lineEndPos = textLen;
        }

        lineText = text.substr(lineStartPos, lineEndPos - lineStartPos);
        if (start > static_cast<int>(lineText.length()))
        {
            start = static_cast<int>(lineText.length());
        }
        end = start;

        while (start > 0)
        {
            startChar = static_cast<unsigned char>(lineText[start - 1]);
            if (SAFE_IS_ALNUM(startChar) || lineText[start - 1] == '_')
            {
                start--;
            }
            else
            {
                break;
            }
        }

        while (end < static_cast<int>(lineText.length()))
        {
            endChar = static_cast<unsigned char>(lineText[end]);
            if (SAFE_IS_ALNUM(endChar) || lineText[end] == '_')
            {
                end++;
            }
            else
            {
                break;
            }
        }

        return lineText.substr(start, end - start);
    }

    /**
     * @brief Collects and populates script classes and object structures compiled inside a module using an accelerated cache lookup layer.
     * @param engine Pointer to the active AngelScript core native compilation environment.
     * @param mod Pointer to the target active script storage segment module.
     * @param customClasses Extracted metadata structure tracking array collection reference.
     */
    void PopulateCustomClassesFromModule(asIScriptEngine *engine, asIScriptModule *mod, std::vector<TokenHarvester::ScriptClass> &customClasses)
    {
        std::unordered_set<std::string_view> existingClasses;
        asUINT objectTypeCount = 0;
        asUINT t = 0;
        asITypeInfo *typeInfo = nullptr;
        std::string_view className = "";
        TokenHarvester::ScriptClass extClass;
        asUINT propertyCount = 0;
        asUINT p = 0;
        const char *pName = nullptr;
        int pTypeId = 0;
        const char *pDecl = nullptr;
        asUINT methodCount = 0;
        asUINT m = 0;
        asIScriptFunction *method = nullptr;
        const char *rDecl = nullptr;
        bool isConstructorOrDestructor = false;
        asUINT enumCount = 0;
        asUINT e = 0;
        asITypeInfo *enumType = nullptr;
        std::string_view enumName = "";
        std::vector<TokenHarvester::ScriptClass>::iterator it;
        asUINT enumValueCount = 0;
        asUINT v = 0;
        asINT64 val = 0;
        const char *enumValName = nullptr;
        TokenHarvester::ScriptClass extEnum;

        if (!mod || !engine)
        {
            return;
        }

        existingClasses.reserve(customClasses.size());
        for (const auto &c : customClasses)
        {
            existingClasses.insert(c.name);
        }

        objectTypeCount = mod->GetObjectTypeCount();
        for (t = 0; t < objectTypeCount; t++)
        {
            typeInfo = mod->GetObjectTypeByIndex(t);
            if (!typeInfo)
            {
                continue;
            }

            className = typeInfo->GetName();
            if (existingClasses.find(className) == existingClasses.end())
            {
                extClass = TokenHarvester::ScriptClass();
                extClass.name = std::string(className);

                propertyCount = typeInfo->GetPropertyCount();
                for (p = 0; p < propertyCount; p++)
                {
                    pName = nullptr;
                    pTypeId = 0;
                    typeInfo->GetProperty(p, &pName, &pTypeId);
                    pDecl = engine->GetTypeDeclaration(pTypeId, true);

                    if (pName && pDecl)
                    {
                        extClass.properties.push_back({pName, pDecl, "public"});
                    }
                }

                methodCount = typeInfo->GetMethodCount();
                for (m = 0; m < methodCount; m++)
                {
                    method = typeInfo->GetMethodByIndex(m);
                    if (method)
                    {
                        rDecl = engine->GetTypeDeclaration(method->GetReturnTypeId(), true);
                        isConstructorOrDestructor = (method->GetName() == className || method->GetName() == ("~" + std::string(className)));

                        extClass.methods.push_back({method->GetName(),
                                                    rDecl ? rDecl : "void",
                                                    method->GetDeclaration(true, false, true),
                                                    "public",
                                                    isConstructorOrDestructor});
                    }
                }

                customClasses.push_back(extClass);
                existingClasses.insert(customClasses.back().name);
            }
        }

        enumCount = mod->GetEnumCount();
        for (e = 0; e < enumCount; e++)
        {
            enumType = mod->GetEnumByIndex(e);
            if (!enumType)
            {
                continue;
            }

            enumName = enumType->GetName();
            if (existingClasses.find(enumName) != existingClasses.end())
            {
                it = std::find_if(customClasses.begin(), customClasses.end(),
                                  [&](const TokenHarvester::ScriptClass &c)
                                  { return c.name == enumName; });

                if (it != customClasses.end())
                {
                    it->properties.clear();
                    it->methods.clear();

                    enumValueCount = enumType->GetEnumValueCount();
                    for (v = 0; v < enumValueCount; v++)
                    {
                        val = 0;
                        enumValName = enumType->GetEnumValueByIndex(v, &val);
                        if (enumValName)
                        {
                            it->properties.push_back({enumValName, std::string(enumName), "public"});
                        }
                    }
                }
            }
            else
            {
                extEnum = TokenHarvester::ScriptClass();
                extEnum.name = std::string(enumName);

                enumValueCount = enumType->GetEnumValueCount();
                for (v = 0; v < enumValueCount; v++)
                {
                    val = 0;
                    enumValName = enumType->GetEnumValueByIndex(v, &val);
                    if (enumValName)
                    {
                        extEnum.properties.push_back({enumValName, std::string(enumName), "public"});
                    }
                }

                customClasses.push_back(extEnum);
                existingClasses.insert(customClasses.back().name);
            }
        }
    }

    /**
     * @brief Looks up custom object types and enums declared within the current active module scope.
     * @param mod Pointer to the active AngelScript script module.
     * @param name Name of the identifier to verify.
     * @return True if the identifier matches a custom object type or enum; false otherwise.
     */
    bool IsCustomScriptType(asIScriptModule *mod, const std::string &name)
    {
        asUINT objectTypeCount = 0;
        asUINT t = 0;
        asITypeInfo *ti = nullptr;
        asUINT enumCount = 0;
        asUINT e = 0;

        if (!mod)
        {
            return false;
        }

        objectTypeCount = mod->GetObjectTypeCount();
        for (t = 0; t < objectTypeCount; t++)
        {
            ti = mod->GetObjectTypeByIndex(t);
            if (ti)
            {
                if (ti->GetName() == name)
                {
                    return true;
                }
            }
        }

        enumCount = mod->GetEnumCount();
        for (e = 0; e < enumCount; e++)
        {
            ti = mod->GetEnumByIndex(e);
            if (ti)
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
     * @param engine Pointer to the active AngelScript core engine framework context.
     * @param mod Pointer to the active script storage segment module.
     * @param name Name string representing the candidate keyword sequence.
     * @return True if validation confirms engine registration or native module symbol maps; false otherwise.
     */
    bool IsEngineOrScriptType(asIScriptEngine *engine, asIScriptModule *mod, const std::string &name)
    {
        if (engine && engine->GetTypeIdByDecl(name.c_str()) >= 0)
        {
            return true;
        }

        return IsCustomScriptType(mod, name);
    }

    /**
     * @brief Evaluation predicate determining if an incoming method matches LSP document lifecycle notifications.
     * @param method The method identifier string view extracted from the JSON-RPC frame payload.
     * @return True if method matches state tracking triggers; false otherwise.
     */
    bool IsLspNotification(std::string_view method) noexcept
    {
        return method == LspConstants::MethodDidOpen || method == LspConstants::MethodDidChange;
    }

    /**
     * @brief Scans a source line character range to isolate and extract path boundaries of preprocessor include statements.
     * @param line Raw script character range segment view.
     * @param outPathStart Receives the precise absolute string block beginning index coordinate offset.
     * @param outPathEnd Receives the precise absolute string block terminal boundary index coordinate offset.
     * @return True if a correctly formed preprocessor include sequence was resolved; false otherwise.
     */
    bool IsIncludeDirective(std::string_view line, size_t &outPathStart, size_t &outPathEnd) noexcept
    {
        size_t firstChar = 0;
        size_t includePos = 0;
        bool validIncludeKeyword = true;
        size_t check = 0;
        size_t startDelim = 0;
        char closeChar = 0;
        size_t endDelim = 0;

        firstChar = line.find_first_not_of(" \t\r\n");
        if (firstChar == std::string::npos || line[firstChar] != '#')
        {
            return false;
        }

        includePos = line.find(LspConstants::IncludeKeyword, firstChar + 1);
        if (includePos == std::string::npos)
        {
            return false;
        }

        for (check = firstChar + 1; check < includePos; ++check)
        {
            if (line[check] != ' ' && line[check] != '\t')
            {
                validIncludeKeyword = false;
                break;
            }
        }

        if (!validIncludeKeyword)
        {
            return false;
        }

        startDelim = line.find_first_of("\"<", includePos + LspConstants::IncludeKeyword.length());
        if (startDelim == std::string::npos)
        {
            return false;
        }

        closeChar = (line[startDelim] == '"') ? '"' : '>';
        endDelim = line.find(closeChar, startDelim + 1);
        if (endDelim == std::string::npos)
        {
            return false;
        }

        outPathStart = startDelim + 1;
        outPathEnd = endDelim;
        return true;
    }
}

/**
 * @brief Launches the main execution loop monitoring client stream traffic blocks and handling routing requests.
 */
void AngelScriptLSPServer::Run()
{
    int contentLength = 0;
    std::string line;
    std::string content = "";
    std::string_view prefix = LspConstants::ContentLengthPrefix;
    json request;
    std::string method = "";
    std::string uri = "";
    std::string text = "";
    json id;
    int l = 0;
    int c = 0;

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    while (true)
    {
        contentLength = 0;
        content = "";

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
            request = json::parse(content);

            if (request.contains("method"))
            {
                method = request["method"].get<std::string>();

                if (method == LspConstants::MethodInitialize)
                {
                    HandleInitialize(request["id"]);
                }
                else if (LspServer::IsLspNotification(method))
                {
                    uri = request["params"]["textDocument"]["uri"].get<std::string>();
                    text = (method == LspConstants::MethodDidOpen)
                               ? request["params"]["textDocument"]["text"].get<std::string>()
                               : request["params"]["contentChanges"][0]["text"].get<std::string>();

                    documentCache[uri] = text;
                    AnalyzeAndReport(uri, text);
                }
                else if (method == LspConstants::MethodSemanticTokens)
                {
                    uri = request["params"]["textDocument"]["uri"].get<std::string>();

                    if (documentCache.find(uri) != documentCache.end())
                    {
                        HandleSemanticTokens(request["id"], uri);
                    }
                }
                else if (method == LspConstants::MethodCompletion)
                {
                    id = request["id"];
                    uri = request["params"]["textDocument"]["uri"].get<std::string>();
                    l = request["params"]["position"]["line"].get<int>();
                    c = request["params"]["position"]["character"].get<int>();

                    if (documentCache.find(uri) != documentCache.end())
                    {
                        HandleCompletion(id, uri, l, c);
                    }
                }
                else if (method == LspConstants::MethodHover)
                {
                    id = request["id"];
                    uri = request["params"]["textDocument"]["uri"].get<std::string>();
                    l = request["params"]["position"]["line"].get<int>();
                    c = request["params"]["position"]["character"].get<int>();

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
 * @brief Serializes a JSON message and sends it to the client output stream with proper LSP framing layout structures.
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
 * @brief Transforms a given file URI into an absolute filesystem path string using fast bitwise hex conversions.
 */
std::string AngelScriptLSPServer::TransformUriToPath(std::string_view uri)
{
    std::string_view remaining = uri;
    std::string decoded = "";
    size_t i = 0;
    size_t len = 0;
    int highNibble = 0;
    int lowNibble = 0;
    char hexChar1 = 0;
    char hexChar2 = 0;

    if (remaining.starts_with(LspConstants::UriFilePrefixWin))
    {
        remaining.remove_prefix(LspConstants::UriFilePrefixWin.length());
    }
    else if (remaining.starts_with(LspConstants::UriFilePrefixUnix))
    {
        remaining.remove_prefix(LspConstants::UriFilePrefixUnix.length());
    }

    len = remaining.length();
    decoded.reserve(len);

    for (i = 0; i < len; ++i)
    {
        if (remaining[i] == '%' && i + 2 < len)
        {
            hexChar1 = remaining[i + 1];
            hexChar2 = remaining[i + 2];

            highNibble = (hexChar1 >= '0' && hexChar1 <= '9') ? (hexChar1 - '0') : (hexChar1 >= 'a' && hexChar1 <= 'f') ? (hexChar1 - 'a' + 10)
                                                                               : (hexChar1 >= 'A' && hexChar1 <= 'F')   ? (hexChar1 - 'A' + 10)
                                                                                                                        : -1;

            lowNibble = (hexChar2 >= '0' && hexChar2 <= '9') ? (hexChar2 - '0') : (hexChar2 >= 'a' && hexChar2 <= 'f') ? (hexChar2 - 'a' + 10)
                                                                              : (hexChar2 >= 'A' && hexChar2 <= 'F')   ? (hexChar2 - 'A' + 10)
                                                                                                                       : -1;

            if (highNibble != -1 && lowNibble != -1)
            {
                decoded += static_cast<char>((highNibble << 4) | lowNibble);
                i += 2;
            }
            else
            {
                decoded += remaining[i];
            }
        }
        else
        {
            decoded += remaining[i];
        }
    }

    return decoded;
}

/**
 * @brief Handles the 'initialize' LSP request by responding with the server's capabilities and supported features.
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
    std::string cleanUri = "";
    json diagnosticsArray = json::array();
    fs::path currentPath;
    fs::path baseDir;
    std::string_view remainingCode = code;
    size_t lineStartPos = 0;
    size_t lineEndPos = 0;
    int lineIndex = 0;
    std::string_view lineStr = "";
    size_t pathStart = 0;
    size_t pathEnd = 0;
    std::string includeFileName = "";
    fs::path absolutePath;
    int line = 0;
    int col = 0;

    cleanUri = TransformUriToPath(uri);
    currentPath = fs::path(cleanUri);
    baseDir = currentPath.parent_path();

    scriptEngine.ClearDiagnostics();
    scriptEngine.BuildModule("LSPModule", cleanUri, code);

    for (const auto &diag : scriptEngine.GetDiagnostics())
    {
        line = std::max(0, diag.row - 1);
        col = std::max(0, diag.col - 1);

        diagnosticsArray.push_back({{"range", {{"start", {{"line", line}, {"character", col}}}, {"end", {{"line", line}, {"character", col + 5}}}}},
                                    {"severity", (diag.type == asMSGTYPE_WARNING) ? 2 : 1},
                                    {"message", diag.message},
                                    {"source", std::string(LspConstants::DiagnosticSource)}});
    }

    while (lineStartPos < remainingCode.length())
    {
        lineEndPos = remainingCode.find('\n', lineStartPos);
        if (lineEndPos == std::string_view::npos)
        {
            lineEndPos = remainingCode.length();
        }

        lineStr = remainingCode.substr(lineStartPos, lineEndPos - lineStartPos);

        if (LspServer::IsIncludeDirective(lineStr, pathStart, pathEnd))
        {
            includeFileName = std::string(lineStr.substr(pathStart, pathEnd - pathStart));
            absolutePath = baseDir / fs::path(includeFileName);

            if (!fs::exists(absolutePath))
            {
                diagnosticsArray.push_back({{"range", {{"start", {{"line", lineIndex}, {"character", 0}}}, {"end", {{"line", lineIndex}, {"character", static_cast<int>(lineStr.length())}}}}},
                                            {"severity", 1},
                                            {"message", fmt::format("Preprocessor Error: Include file '{}' not found on path.", includeFileName)},
                                            {"source", std::string(LspConstants::PreprocessorSource)}});
            }
        }

        lineStartPos = lineEndPos + 1;
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
    asIScriptEngine *nativeEng = nullptr;
    asIScriptModule *mod = nullptr;
    asUINT len = 0;
    asETokenClass tc = asTC_UNKNOWN;
    int tokenType = -1;
    std::string_view text = "";
    std::string textStr = "";
    size_t nextPos = 0;
    int deltaLine = 0;
    int deltaChar = 0;
    asUINT j = 0;

    nativeEng = scriptEngine.GetNativeEngine();
    mod = nativeEng->GetModule("LSPModule");

    while (i < code.length())
    {
        len = 0;
        tc = nativeEng->ParseToken(code.data() + i, static_cast<asUINT>(code.length() - i), &len);
        tokenType = -1;

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
            text = code.substr(i, len);
            textStr = std::string(text);

            if (LspServer::IsEngineOrScriptType(nativeEng, mod, textStr))
            {
                tokenType = 1;
            }
            else
            {
                tokenType = 3;
                nextPos = i + len;

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
            deltaLine = currentLine - prevLine;
            deltaChar = (deltaLine == 0) ? (currentChar - prevChar) : currentChar;

            tokens.push_back(deltaLine);
            tokens.push_back(deltaChar);
            tokens.push_back(len);
            tokens.push_back(tokenType);
            tokens.push_back(0);

            prevLine = currentLine;
            prevChar = currentChar;
        }

        for (j = 0; j < len; ++j)
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
 * @brief Processes completion context trees and calculates in-scope structural data lists using stack-bound contexts.
 */
void AngelScriptLSPServer::HandleCompletion(json id, const std::string &uri, int line, int character)
{
    std::string originalText = "";
    asIScriptEngine *nativeEng = nullptr;
    size_t cursorAbsPos = 0;
    TokenHarvester::CompletionContext ctx;
    std::string modifiedText = "";
    std::string cleanUri = "";
    asIScriptModule *mod = nullptr;
    std::string enclosingClass = "";
    std::vector<TokenHarvester::ScriptClass> customClasses;
    std::vector<TokenHarvester::GlobalFunction> tokenFuncs;
    std::vector<TokenHarvester::GlobalVariable> tokenGlobalVars;
    std::vector<TokenHarvester::LocalVariable> localVars;
    std::function<void(const std::string &)> logger = [](const std::string &msg)
    {
        std::cerr << "[AST Debug] " << msg << std::endl;
    };
    std::optional<CompletionHandler> handler;
    json itemsArray;
    json response;

    originalText = documentCache[uri];
    nativeEng = scriptEngine.GetNativeEngine();
    cursorAbsPos = TokenHarvester::GetAbsolutePosition(originalText, line, character);
    ctx = TokenHarvester::GetCompletionContext(nativeEng, originalText, cursorAbsPos);
    modifiedText = LspServer::BlankOutLine(originalText, line);
    cleanUri = TransformUriToPath(uri);

    scriptEngine.BuildModule("LSPModule", cleanUri, modifiedText);
    mod = nativeEng->GetModule("LSPModule");
    customClasses = TokenHarvester::ScanCustomClasses(nativeEng, originalText);

    LspServer::PopulateCustomClassesFromModule(nativeEng, mod, customClasses);

    tokenFuncs = TokenHarvester::ScanGlobalFunctions(nativeEng, originalText);
    tokenGlobalVars = TokenHarvester::ScanGlobalVariables(nativeEng, originalText);
    localVars = TokenHarvester::ScanLocalVariables(nativeEng, originalText, cursorAbsPos, enclosingClass, customClasses, tokenGlobalVars, tokenFuncs);

    std::ofstream("angelscript_ast_debug.txt", std::ios_base::trunc).close();

    handler.emplace(nativeEng, mod, ctx, enclosingClass, localVars, customClasses, tokenFuncs, tokenGlobalVars, logger);
    itemsArray = handler->GenerateItems(originalText, cursorAbsPos);
    response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isIncomplete", false}, {"items", itemsArray}}}};

    SendToVSCode(response);
}

/**
 * @brief Active syntactic scope interrogation handler matching AngelScript's official EBNF grammar rules.
 */
void AngelScriptLSPServer::HandleHover(json id, const std::string &uri, int line, int character)
{
    std::string originalText = "";
    asIScriptEngine *nativeEng = nullptr;
    std::optional<HoverHandler> handler;
    json hoverResult;
    json response;

    originalText = documentCache[uri];
    nativeEng = scriptEngine.GetNativeEngine();

    handler.emplace(nativeEng, originalText, line, character);
    hoverResult = handler->Process();

    response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", hoverResult}};
    SendToVSCode(response);
}