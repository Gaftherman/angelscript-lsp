/**
 * @file LspServer.cpp
 * @brief Implements JSON-RPC message parsing and routing workflows compliant with LSP lifecycle rules.
 * @author AngelScript LSP Team
 */

#include "LspServer.h"
#include "TokenHarvester.h"
#include "CompletionHandler.h"
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

    static const std::unordered_set<std::string> reservedKeywords = {
        "and", "auto", "bool", "break", "case", "cast", "catch", "class", "const", "continue",
        "default", "do", "double", "else", "enum", "false", "float", "for", "foreach", "funcdef",
        "if", "import", "in", "inout", "int", "interface", "int8", "int16", "int32", "int64",
        "is", "mixin", "namespace", "not", "null", "or", "out", "private", "protected", "return",
        "switch", "true", "try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64", "using",
        "void", "while", "xor"};

    static const std::unordered_set<std::string> contextualKeywords = {
        "abstract", "delete", "explicit", "external", "final", "from", "function", "get",
        "override", "property", "set", "shared", "super", "this"};

    static const std::unordered_set<std::string> primitiveTypes = {
        "void", "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64", "float", "double", "bool", "auto"};

    static const std::unordered_set<std::string> storageModifiers = {
        "private", "protected", "public", "shared", "external"};

    struct TokenInfo
    {
        std::string text;
        asETokenClass tokenClass;
        size_t startPos;
        size_t endPos;
        int line;
        int character;
    };

    struct DeclInfo
    {
        std::string name;
        std::string fullName;
        std::string type;
        std::string hoverText;
        size_t startPos = 0;
        size_t endPos = std::string::npos;
    };

    struct ScopeRange
    {
        std::string type;
        std::string name;
        std::string fullName;
        size_t startPos;
        size_t endPos;
    };

    struct ScopeFrame
    {
        std::string type;
        std::string name;
        size_t openBraceIdx;
        size_t startPos;
    };

    std::vector<TokenInfo> allTokens;
    std::vector<DeclInfo> declarations;
    std::vector<ScopeRange> structuralScopes;
    std::vector<ScopeFrame> openFrames;
    std::unordered_map<std::string, std::vector<std::string>> classInheritanceMapper;

    int curLine = 0;
    int curChar = 0;
    size_t pos = 0;
    int targetIdx = -1;
    size_t idxScan = 0;
    std::string hoverResult = "";

    // 1. Base Tokenization Phase
    while (pos < originalText.length())
    {
        asUINT len = 0;
        asETokenClass tc = nativeEng->ParseToken(originalText.data() + pos, static_cast<asUINT>(originalText.length() - pos), &len);

        if (len == 0)
        {
            len = 1;
            tc = asTC_UNKNOWN;
        }

        std::string tokText = originalText.substr(pos, len);

        if (tc == asTC_WHITESPACE || tc == asTC_COMMENT)
        {
            for (char c : tokText)
            {
                if (c == '\n')
                {
                    curLine++;
                    curChar = 0;
                }
                else
                {
                    curChar++;
                }
            }

            pos += len;
            continue;
        }

        allTokens.push_back({tokText, tc, pos, pos + len, curLine, curChar});

        for (char c : tokText)
        {
            if (c == '\n')
            {
                curLine++;
                curChar = 0;
            }
            else
            {
                curChar++;
            }
        }

        pos += len;
    }

    for (int k = 0; k < (int)allTokens.size(); ++k)
    {
        if (allTokens[k].line == line && character >= allTokens[k].character && character <= allTokens[k].character + (int)allTokens[k].text.length())
        {
            targetIdx = k;
            break;
        }
    }

    auto IsWord = [](const std::string &s) -> bool
    {
        if (s.empty())
        {
            return false;
        }

        char c = s[0];
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    auto CleanSignature = [](std::string str) -> std::string
    {
        std::string res = "";
        std::string finalRes = "";
        size_t p = 0;

        for (size_t idx = 0; idx < str.size(); ++idx)
        {
            char c = str[idx];

            if (c == ' ')
            {
                if (!res.empty() && (res.back() == '<' || res.back() == '>' || res.back() == '@' || res.back() == '&' || res.back() == ':' || res.back() == '('))
                {
                    continue;
                }

                if (idx + 1 < str.size() && (str[idx + 1] == '<' || str[idx + 1] == '>' || str[idx + 1] == '@' || str[idx + 1] == '&' || str[idx + 1] == ':' || str[idx + 1] == ',' || str[idx + 1] == ')' || str[idx + 1] == '('))
                {
                    continue;
                }
            }

            res += c;
        }

        for (size_t idx = 0; idx < res.size(); ++idx)
        {
            finalRes += res[idx];

            if (res[idx] == '@' || res[idx] == '&' || res[idx] == ',')
            {
                if (idx + 1 < res.size() && res[idx + 1] != ' ' && res[idx + 1] != ',' && res[idx + 1] != ')' && res[idx + 1] != '>')
                {
                    finalRes += ' ';
                }
            }

            if (res[idx] == '>')
            {
                if (idx + 1 < res.size() && res[idx + 1] != ' ' && res[idx + 1] != ',' && res[idx + 1] != ')' && res[idx + 1] != '>' && res[idx + 1] != '@' && res[idx + 1] != '&')
                {
                    finalRes += ' ';
                }
            }
        }

        while ((p = finalRes.find("& in")) != std::string::npos)
        {
            finalRes.replace(p, 4, "&in");
        }

        while ((p = finalRes.find("& out")) != std::string::npos)
        {
            finalRes.replace(p, 5, "&out");
        }

        while ((p = finalRes.find("& inout")) != std::string::npos)
        {
            finalRes.replace(p, 7, "&inout");
        }

        return finalRes;
    };

    auto ParseType = [&](size_t startIdx, size_t &nextIdx, std::string &typeStr) -> bool
    {
        size_t idx = startIdx;
        std::vector<size_t> typeTokenIndices;
        int continuousWords = 0;

        if (idx >= allTokens.size())
        {
            return false;
        }

        while (idx < allTokens.size() && (storageModifiers.count(allTokens[idx].text) || allTokens[idx].text == "shared" || allTokens[idx].text == "external"))
        {
            idx++;
        }

        if (idx >= allTokens.size())
        {
            return false;
        }

        if (allTokens[idx].text == "const")
        {
            typeStr += "const ";
            idx++;
        }

        while (idx < allTokens.size())
        {
            std::string txt = allTokens[idx].text;

            if (IsWord(txt) || txt == "::" || txt == "?")
            {
                typeTokenIndices.push_back(idx);
                idx++;
            }
            else
            {
                break;
            }
        }

        if (typeTokenIndices.empty())
        {
            return false;
        }

        if (allTokens[typeTokenIndices.back()].text == "::")
        {
            return false;
        }

        for (int k = (int)typeTokenIndices.size() - 1; k >= 0; --k)
        {
            if (IsWord(allTokens[typeTokenIndices[k]].text) && allTokens[typeTokenIndices[k]].text != "::")
            {
                continuousWords++;
            }
            else
            {
                break;
            }
        }

        if (continuousWords >= 2)
        {
            size_t actualTypeEndCount = typeTokenIndices.size() - 1;
            idx = typeTokenIndices[actualTypeEndCount];
            typeTokenIndices.resize(actualTypeEndCount);
        }

        if (typeTokenIndices.empty())
        {
            return false;
        }

        for (size_t tIdx : typeTokenIndices)
        {
            typeStr += allTokens[tIdx].text;
        }

        if (idx < allTokens.size() && allTokens[idx].text == "<")
        {
            std::string tempTemplateStr = "<";
            size_t templateIdx = idx + 1;
            int depth = 1;
            bool isLegitTemplate = true;

            while (templateIdx < allTokens.size() && depth > 0)
            {
                std::string txt = allTokens[templateIdx].text;

                if (txt == "<")
                {
                    depth++;
                }
                else if (txt == ">")
                {
                    depth--;
                }
                else if (IsWord(txt) || txt == "::" || txt == "@" || txt == "&" || txt == "," || txt == "[" || txt == "]" || txt == "?")
                {
                }
                else
                {
                    isLegitTemplate = false;
                    break;
                }

                tempTemplateStr += txt;
                templateIdx++;
            }

            if (!isLegitTemplate || depth > 0)
            {
                return false;
            }

            typeStr += tempTemplateStr;
            idx = templateIdx;
        }

        while (idx < allTokens.size())
        {
            if (allTokens[idx].text == "[")
            {
                if (idx + 1 < allTokens.size() && allTokens[idx + 1].text == "]")
                {
                    typeStr += "[]";
                    idx += 2;
                }
                else
                {
                    break;
                }
            }
            else if (allTokens[idx].text == "@")
            {
                typeStr += "@";
                idx++;

                if (idx < allTokens.size() && allTokens[idx].text == "const")
                {
                    typeStr += " const";
                    idx++;
                }
            }
            else if (allTokens[idx].text == "&")
            {
                typeStr += "&";
                idx++;

                if (idx < allTokens.size() && (allTokens[idx].text == "in" || allTokens[idx].text == "out" || allTokens[idx].text == "inout"))
                {
                    typeStr += allTokens[idx].text;
                    idx++;
                }
            }
            else
            {
                break;
            }
        }

        nextIdx = idx;
        return true;
    };

    // 2. Structural Parsing Pass (Scope Resolution Tracking & Inheritance Mapping)
    std::vector<std::string> tokenScopePrefixes(allTokens.size(), "");

    for (size_t k = 0; k < allTokens.size(); ++k)
    {
        std::string currentPrefix = "";

        for (const auto &frame : openFrames)
        {
            if (frame.type == "namespace" || frame.type == "class" || frame.type == "interface" || frame.type == "mixin class" || frame.type == "abstract class")
            {
                currentPrefix += frame.name + "::";
            }
        }

        tokenScopePrefixes[k] = currentPrefix;

        if (allTokens[k].text == "{")
        {
            int look = (int)k - 1;
            std::string fType = "other";
            std::string fName = "";
            bool hasParen = false;

            while (look >= 0)
            {
                std::string t = allTokens[look].text;

                if (t == "}" || t == ";")
                {
                    break;
                }

                if (t == ")")
                {
                    hasParen = true;
                }

                if (t == "namespace" || t == "class" || t == "interface" || t == "enum")
                {
                    fType = t;

                    if (look + 1 < (int)allTokens.size())
                    {
                        fName = allTokens[look + 1].text;
                    }

                    if (t == "class" && look > 0 && allTokens[look - 1].text == "mixin")
                    {
                        fType = "mixin class";
                    }

                    if (t == "class" && look > 0 && allTokens[look - 1].text == "abstract")
                    {
                        fType = "abstract class";
                    }

                    if (t == "class" || t == "interface")
                    {
                        size_t inheritScan = look + 2;

                        while (inheritScan < k)
                        {
                            std::string tokenText = allTokens[inheritScan].text;

                            if (IsWord(tokenText) && !storageModifiers.count(tokenText))
                            {
                                classInheritanceMapper[fName].push_back(tokenText);
                            }

                            inheritScan++;
                        }
                    }

                    break;
                }

                look--;
            }

            if (fType == "other" && hasParen)
            {
                int sp = (int)k - 1;

                while (sp >= 0 && allTokens[sp].text != "(")
                {
                    sp--;
                }

                if (sp > 0 && IsWord(allTokens[sp - 1].text))
                {
                    std::string possibleFuncName = allTokens[sp - 1].text;

                    if (possibleFuncName != "if" && possibleFuncName != "for" && possibleFuncName != "foreach" &&
                        possibleFuncName != "while" && possibleFuncName != "switch" && possibleFuncName != "catch" &&
                        possibleFuncName != "cast" && possibleFuncName != "function")
                    {
                        fType = "function";
                        fName = possibleFuncName;

                        if (sp >= 2 && allTokens[sp - 2].text == "~")
                        {
                            fName = "~" + fName;
                        }
                    }
                }
            }

            openFrames.push_back({fType, fName, k, allTokens[k].startPos});

            if (fType != "other" && fType != "function")
            {
                declarations.push_back({fName, currentPrefix + fName, fType, fType + " " + currentPrefix + fName, allTokens[k].startPos, std::string::npos});

                if (fType == "enum")
                {
                    size_t enumScan = k + 1;
                    int enumVal = 0;

                    while (enumScan < allTokens.size() && allTokens[enumScan].text != "}")
                    {
                        if (allTokens[enumScan].tokenClass == asTC_IDENTIFIER && IsWord(allTokens[enumScan].text))
                        {
                            declarations.push_back({allTokens[enumScan].text, currentPrefix + fName + "::" + allTokens[enumScan].text, "enum_value", "enum " + currentPrefix + fName + "::" + allTokens[enumScan].text + " = " + std::to_string(enumVal), allTokens[enumScan].startPos, allTokens[enumScan].endPos});
                            enumVal++;
                        }

                        enumScan++;
                    }
                }
            }
        }
        else if (allTokens[k].text == "}")
        {
            if (!openFrames.empty())
            {
                auto top = openFrames.back();
                openFrames.pop_back();

                if (top.type != "other")
                {
                    structuralScopes.push_back({top.type, top.name, tokenScopePrefixes[top.openBraceIdx] + top.name, top.startPos, allTokens[k].endPos});

                    for (auto &decl : declarations)
                    {
                        if (decl.name == top.name && decl.type == top.type && decl.startPos == top.startPos)
                        {
                            decl.endPos = allTokens[k].endPos;
                            break;
                        }
                    }
                }
            }
        }
    }

    // 3. Third Pass: Extraction of Class Properties, Functions, and Declarations
    while (idxScan < allTokens.size())
    {
        std::string txt = allTokens[idxScan].text;
        std::string currentPrefix = tokenScopePrefixes[idxScan];

        if (txt == "class" || txt == "interface" || txt == "namespace" || txt == "enum" || txt == "mixin" || txt == "abstract")
        {
            idxScan++;
            continue;
        }

        if (storageModifiers.count(txt))
        {
            idxScan++;
            continue;
        }

        if (txt == "typedef" && idxScan + 2 < allTokens.size())
        {
            declarations.push_back({allTokens[idxScan + 2].text, allTokens[idxScan + 2].text, "typedef", "typedef " + allTokens[idxScan + 1].text + " " + allTokens[idxScan + 2].text, allTokens[idxScan + 2].startPos, allTokens[idxScan + 2].endPos});
            idxScan += 3;
            continue;
        }

        if (txt == "funcdef")
        {
            size_t nextIdx = idxScan + 1;
            std::string typeStr = "";

            if (ParseType(idxScan + 1, nextIdx, typeStr))
            {
                if (nextIdx < allTokens.size() && allTokens[nextIdx].text == "@")
                {
                    nextIdx++;
                }

                if (nextIdx < allTokens.size() && IsWord(allTokens[nextIdx].text))
                {
                    std::string fdName = allTokens[nextIdx].text;
                    std::string fullDecl = "funcdef " + typeStr + " " + fdName;
                    size_t paramScan = nextIdx + 1;

                    while (paramScan < allTokens.size() && allTokens[paramScan].text != ";")
                    {
                        fullDecl += " " + allTokens[paramScan].text;
                        paramScan++;
                    }

                    size_t endP = (paramScan < allTokens.size()) ? allTokens[paramScan].endPos : allTokens[paramScan - 1].endPos;

                    declarations.push_back({fdName, currentPrefix + fdName, "funcdef", fullDecl, allTokens[nextIdx].startPos, endP});
                    idxScan = (paramScan < allTokens.size()) ? paramScan + 1 : paramScan;
                    continue;
                }
            }
        }

        size_t nextIdx = idxScan;
        std::string typeStr = "";

        if (ParseType(idxScan, nextIdx, typeStr))
        {
            if (nextIdx <= idxScan)
            {
                idxScan++;
                continue;
            }

            std::string baseType = allTokens[idxScan].text;

            if (baseType == "return" || baseType == "if" || baseType == "for" || baseType == "while" ||
                baseType == "break" || baseType == "continue" || baseType == "switch" || baseType == "case" ||
                baseType == "default" || baseType == "cast" || baseType == "try" || baseType == "catch" ||
                baseType == "delete" || baseType == "throw")
            {
                idxScan++;
                continue;
            }

            bool statementMatched = false;
            size_t innerScan = nextIdx;

            while (innerScan < allTokens.size() && IsWord(allTokens[innerScan].text) && !reservedKeywords.count(allTokens[innerScan].text))
            {
                size_t nameIdx = innerScan;
                std::string entityName = allTokens[nameIdx].text;
                std::string activeFuncName = "";
                size_t activeFuncStart = std::string::npos;
                size_t activeFuncEnd = std::string::npos;
                std::string activeClassName = "";
                size_t activeClassStart = std::string::npos;
                size_t activeClassEnd = std::string::npos;
                size_t entPos = allTokens[nameIdx].startPos;

                for (const auto &scope : structuralScopes)
                {
                    if (entPos >= scope.startPos && entPos <= scope.endPos)
                    {
                        if (scope.type == "function")
                        {
                            if (activeFuncStart == std::string::npos || scope.startPos > activeFuncStart)
                            {
                                activeFuncName = scope.name;
                                activeFuncStart = scope.startPos;
                                activeFuncEnd = scope.endPos;
                            }
                        }
                        else if (scope.type == "class" || scope.type == "interface" || scope.type == "mixin class" || scope.type == "abstract class")
                        {
                            if (activeClassStart == std::string::npos || scope.startPos > activeClassStart)
                            {
                                activeClassName = scope.name;
                                activeClassStart = scope.startPos;
                                activeClassEnd = scope.endPos;
                            }
                        }
                    }
                }

                if (nameIdx + 1 < allTokens.size() && allTokens[nameIdx + 1].text == "(")
                {
                    size_t closeParen = nameIdx + 1;
                    int pDepth = 1;

                    while (closeParen + 1 < allTokens.size() && pDepth > 0)
                    {
                        closeParen++;

                        if (allTokens[closeParen].text == "(")
                            pDepth++;
                        else if (allTokens[closeParen].text == ")")
                            pDepth--;
                    }

                    size_t modScan = closeParen + 1;
                    std::string modifiers = "";

                    while (modScan < allTokens.size() && allTokens[modScan].text != ";" && allTokens[modScan].text != "{" && allTokens[modScan].text != ",")
                    {
                        if (contextualKeywords.count(allTokens[modScan].text) || reservedKeywords.count(allTokens[modScan].text))
                        {
                            modifiers += " " + allTokens[modScan].text;
                        }

                        modScan++;
                    }

                    bool isFunctionDef = false;

                    if (modScan < allTokens.size() && allTokens[modScan].text == "{")
                    {
                        isFunctionDef = true;
                    }
                    else if (activeClassStart != std::string::npos && activeFuncStart == std::string::npos)
                    {
                        isFunctionDef = true;
                    }
                    else if (activeClassStart == std::string::npos && activeFuncStart == std::string::npos && modifiers.find("import") != std::string::npos)
                    {
                        isFunctionDef = true;
                    }

                    if (isFunctionDef)
                    {
                        struct FuncParam
                        {
                            std::string pName;
                            std::string pType;
                        };
                        std::vector<FuncParam> funcParams;
                        std::vector<TokenInfo> pToks;

                        for (size_t k = nameIdx + 2; k < closeParen; ++k)
                        {
                            if (allTokens[k].text == "," || k == closeParen - 1)
                            {
                                if (k == closeParen - 1 && allTokens[k].text != ",")
                                {
                                    pToks.push_back(allTokens[k]);
                                }

                                if (!pToks.empty() && IsWord(pToks.back().text) && !reservedKeywords.count(pToks.back().text))
                                {
                                    TokenInfo nTok = pToks.back();
                                    pToks.pop_back();
                                    std::string ptStr = "";

                                    for (size_t t = 0; t < pToks.size(); ++t)
                                    {
                                        if (t > 0 && pToks[t].text != "@" && pToks[t].text != "&" && pToks[t - 1].text != "::")
                                        {
                                            ptStr += " ";
                                        }

                                        ptStr += pToks[t].text;
                                    }

                                    funcParams.push_back({nTok.text, ptStr});
                                }

                                pToks.clear();
                            }
                            else
                            {
                                pToks.push_back(allTokens[k]);
                            }
                        }

                        size_t functionEndPos = allTokens[closeParen].endPos;

                        if (modScan < allTokens.size() && allTokens[modScan].text == "{")
                        {
                            for (const auto &scope : structuralScopes)
                            {
                                if (scope.type == "function" && scope.name == entityName && scope.startPos == allTokens[modScan].startPos)
                                {
                                    functionEndPos = scope.endPos;
                                    break;
                                }
                            }
                        }

                        std::string paramsStr = "";

                        for (size_t p = 0; p < funcParams.size(); ++p)
                        {
                            if (p > 0)
                            {
                                paramsStr += ", ";
                            }

                            paramsStr += funcParams[p].pType + " " + funcParams[p].pName;
                        }

                        std::string fullQualFunc = currentPrefix + entityName;
                        declarations.push_back({entityName, fullQualFunc, "function", typeStr + " " + fullQualFunc + "(" + paramsStr + ")" + modifiers, allTokens[nameIdx].startPos, functionEndPos});

                        if (functionEndPos != std::string::npos)
                        {
                            for (const auto &fp : funcParams)
                            {
                                declarations.push_back({fp.pName, fp.pName, "parameter", fp.pType + " " + fp.pName, allTokens[nameIdx].startPos, functionEndPos});
                            }
                        }

                        idxScan = (modScan < allTokens.size() && allTokens[modScan].text == "{") ? modScan + 1 : modScan;
                        statementMatched = true;
                        break;
                    }
                    else
                    {
                        if (activeFuncStart != std::string::npos)
                        {
                            declarations.push_back({entityName, entityName, "local_variable", typeStr + " " + entityName, activeFuncStart, activeFuncEnd});
                        }
                        else if (activeClassStart != std::string::npos)
                        {
                            declarations.push_back({entityName, currentPrefix + entityName, "property", typeStr + " " + currentPrefix + entityName, activeClassStart, activeClassEnd});
                        }
                        else
                        {
                            declarations.push_back({entityName, currentPrefix + entityName, "global_variable", typeStr + " " + currentPrefix + entityName, allTokens[nameIdx].startPos, std::string::npos});
                        }

                        size_t searchComma = closeParen + 1;
                        int sqDepth = 0;

                        while (searchComma < allTokens.size())
                        {
                            std::string sTxt = allTokens[searchComma].text;

                            if (sTxt == "(" || sTxt == "[" || sTxt == "{")
                                sqDepth++;
                            else if (sTxt == ")" || sTxt == "]" || sTxt == "}")
                                sqDepth--;
                            else if (sqDepth == 0 && (sTxt == "," || sTxt == ";"))
                                break;

                            searchComma++;
                        }

                        if (searchComma < allTokens.size() && allTokens[searchComma].text == ",")
                        {
                            innerScan = searchComma + 1;
                            continue;
                        }
                        else
                        {
                            idxScan = (searchComma < allTokens.size()) ? searchComma + 1 : searchComma;
                            statementMatched = true;
                            break;
                        }
                    }
                }
                else if (nameIdx + 1 < allTokens.size() && allTokens[nameIdx + 1].text == "{")
                {
                    std::string modifiers = " property";
                    size_t inner = nameIdx + 2;

                    while (inner < allTokens.size() && allTokens[inner].text != "}")
                    {
                        if (allTokens[inner].text == "const" || allTokens[inner].text == "override")
                        {
                            modifiers += " " + allTokens[inner].text;
                        }

                        inner++;
                    }

                    declarations.push_back({entityName, currentPrefix + entityName, "property", typeStr + " " + currentPrefix + entityName + modifiers, allTokens[nameIdx].startPos, allTokens[inner].endPos});
                    idxScan = inner + 1;
                    statementMatched = true;
                    break;
                }
                else if (nameIdx + 1 < allTokens.size() && (allTokens[nameIdx + 1].text == ";" || allTokens[nameIdx + 1].text == "=" || allTokens[nameIdx + 1].text == "," || allTokens[nameIdx + 1].text == "[" || allTokens[nameIdx + 1].text == ")"))
                {
                    if (activeFuncStart != std::string::npos)
                    {
                        declarations.push_back({entityName, entityName, "local_variable", typeStr + " " + entityName, activeFuncStart, activeFuncEnd});
                    }
                    else if (activeClassStart != std::string::npos)
                    {
                        declarations.push_back({entityName, currentPrefix + entityName, "property", typeStr + " " + currentPrefix + entityName, activeClassStart, activeClassEnd});
                    }
                    else
                    {
                        declarations.push_back({entityName, currentPrefix + entityName, "global_variable", typeStr + " " + currentPrefix + entityName, allTokens[nameIdx].startPos, std::string::npos});
                    }

                    size_t searchComma = nameIdx + 1;
                    int sqDepth = 0;

                    while (searchComma < allTokens.size())
                    {
                        std::string sTxt = allTokens[searchComma].text;

                        if (sTxt == "(" || sTxt == "[" || sTxt == "{")
                            sqDepth++;
                        else if (sTxt == ")" || sTxt == "]" || sTxt == "}")
                            sqDepth--;
                        else if (sqDepth == 0 && (sTxt == "," || sTxt == ";"))
                            break;

                        searchComma++;
                    }

                    if (searchComma < allTokens.size() && allTokens[searchComma].text == ",")
                    {
                        innerScan = searchComma + 1;
                        continue;
                    }
                    else
                    {
                        idxScan = (searchComma < allTokens.size()) ? searchComma + 1 : searchComma;
                        statementMatched = true;
                        break;
                    }
                }

                break;
            }

            if (statementMatched)
            {
                continue;
            }

            idxScan = nextIdx;
            continue;
        }

        idxScan++;
    }

    // 4. Fourth Pass: Extraction of Lambda Context Expressions (Anonymous Callbacks)
    for (size_t k = 0; k < allTokens.size(); ++k)
    {
        if (allTokens[k].text == "function" && k + 1 < allTokens.size() && allTokens[k + 1].text == "(")
        {
            size_t nameIdx = k;
            size_t closeParen = nameIdx + 1;
            int pDepth = 1;

            while (closeParen + 1 < allTokens.size() && pDepth > 0)
            {
                closeParen++;

                if (allTokens[closeParen].text == "(")
                    pDepth++;
                else if (allTokens[closeParen].text == ")")
                    pDepth--;
            }

            struct FuncParam
            {
                std::string pName;
                std::string pType;
            };
            std::vector<FuncParam> funcParams;
            std::vector<TokenInfo> pToks;

            for (size_t idxP = nameIdx + 2; idxP < closeParen; ++idxP)
            {
                if (allTokens[idxP].text == "," || idxP == closeParen - 1)
                {
                    if (idxP == closeParen - 1 && allTokens[idxP].text != ",")
                    {
                        pToks.push_back(allTokens[idxP]);
                    }

                    if (!pToks.empty() && IsWord(pToks.back().text) && !reservedKeywords.count(pToks.back().text))
                    {
                        TokenInfo nTok = pToks.back();
                        pToks.pop_back();
                        std::string ptStr = "";

                        for (size_t t = 0; t < pToks.size(); ++t)
                        {
                            if (t > 0 && pToks[t].text != "@" && pToks[t].text != "&" && pToks[t - 1].text != "::")
                            {
                                ptStr += " ";
                            }

                            ptStr += pToks[t].text;
                        }

                        funcParams.push_back({nTok.text, ptStr});
                    }

                    pToks.clear();
                }
                else
                {
                    pToks.push_back(allTokens[idxP]);
                }
            }

            std::string inferredRet = "auto";
            int lookBack = (int)nameIdx - 1;

            while (lookBack >= 0 && (allTokens[lookBack].text == "@" || allTokens[lookBack].text == "=" || allTokens[lookBack].text == "return" || allTokens[lookBack].text == "(" || allTokens[lookBack].text == "," || allTokens[lookBack].tokenClass == asTC_WHITESPACE))
            {
                if (allTokens[lookBack].text == "return")
                {
                    inferredRet = "auto";
                    break;
                }

                lookBack--;
            }

            if (lookBack >= 0 && IsWord(allTokens[lookBack].text))
            {
                std::string targetVar = allTokens[lookBack].text;

                for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                {
                    if (it->name == targetVar && (it->type == "local_variable" || it->type == "property" || it->type == "global_variable" || it->type == "parameter"))
                    {
                        size_t spacePos = it->hoverText.rfind(' ');

                        if (spacePos != std::string::npos)
                        {
                            inferredRet = it->hoverText.substr(0, spacePos);

                            if (inferredRet.find("private ") == 0)
                                inferredRet = inferredRet.substr(8);
                            if (inferredRet.find("protected ") == 0)
                                inferredRet = inferredRet.substr(10);

                            while (!inferredRet.empty() && (inferredRet.back() == '@' || inferredRet.back() == '&'))
                                inferredRet.pop_back();
                        }

                        break;
                    }
                }
            }

            size_t modScan = closeParen + 1;
            size_t bodyBraceIdx = std::string::npos;

            if (modScan < allTokens.size() && allTokens[modScan].text == "{")
            {
                bodyBraceIdx = modScan;
            }

            size_t functionEndPos = allTokens[closeParen].endPos;

            if (bodyBraceIdx != std::string::npos)
            {
                for (const auto &scope : structuralScopes)
                {
                    if (scope.type == "function" && scope.startPos == allTokens[bodyBraceIdx].startPos)
                    {
                        functionEndPos = scope.endPos;
                        break;
                    }
                }
            }

            std::string paramsStr = "";

            for (size_t p = 0; p < funcParams.size(); ++p)
            {
                if (p > 0)
                {
                    paramsStr += ", ";
                }

                paramsStr += funcParams[p].pType + " " + funcParams[p].pName;
            }

            std::string hoverTxt = inferredRet + " function(" + paramsStr + ")";
            declarations.push_back({"function", "function", "lambda", hoverTxt, allTokens[nameIdx].startPos, functionEndPos});

            for (const auto &fp : funcParams)
            {
                declarations.push_back({fp.pName, fp.pName, "parameter", fp.pType + " " + fp.pName, allTokens[nameIdx].startPos, functionEndPos});
            }

            k = closeParen;
        }
    }

    // 5. Final Semantic Validation Pass
    if (targetIdx != -1)
    {
        TokenInfo targetTok = allTokens[targetIdx];
        bool isMemberAccess = (targetIdx >= 1 && allTokens[targetIdx - 1].text == ".");

        if (isMemberAccess && targetTok.tokenClass == asTC_IDENTIFIER)
        {
            std::string objName = "";
            int backIdx = targetIdx - 2;

            if (backIdx >= 0 && allTokens[backIdx].text == "]")
            {
                int depth = 1;
                backIdx--;

                while (backIdx >= 0 && depth > 0)
                {
                    if (allTokens[backIdx].text == "]")
                        depth++;
                    else if (backIdx >= 0 && allTokens[backIdx].text == "[")
                        depth--;

                    backIdx--;
                }
            }

            if (backIdx >= 0 && IsWord(allTokens[backIdx].text))
            {
                objName = allTokens[backIdx].text;
            }

            std::string objType = "";

            for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                const auto &decl = *it;

                if (decl.name == objName && (decl.type == "local_variable" || decl.type == "parameter" || decl.type == "property" || decl.type == "global_variable"))
                {
                    if (decl.type != "local_variable" && decl.type != "parameter" || (targetTok.startPos >= decl.startPos && targetTok.startPos <= decl.endPos))
                    {
                        size_t lastSpace = decl.hoverText.rfind(' ');

                        if (lastSpace != std::string::npos)
                        {
                            objType = decl.hoverText.substr(0, lastSpace);

                            if (objType.find("private ") == 0)
                                objType = objType.substr(8);
                            if (objType.find("protected ") == 0)
                                objType = objType.substr(10);
                        }

                        break;
                    }
                }
            }

            if (!objType.empty())
            {
                std::string cleanTypeName = "";

                for (char c : objType)
                {
                    if (c == '@' || c == '&' || c == ' ' || c == '<')
                    {
                        break;
                    }

                    cleanTypeName += c;
                }

                std::vector<std::string> searchHierarchy = {cleanTypeName};

                if (classInheritanceMapper.count(cleanTypeName))
                {
                    for (const auto &baseClass : classInheritanceMapper[cleanTypeName])
                    {
                        searchHierarchy.push_back(baseClass);
                    }
                }

                bool methodResolved = false;

                for (const std::string &currentType : searchHierarchy)
                {
                    for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                    {
                        const auto &decl = *it;

                        if (decl.name == targetTok.text && (decl.type == "function" || decl.type == "property"))
                        {
                            if (decl.fullName.find(currentType + "::") != std::string::npos)
                            {
                                hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(decl.hoverText));
                                methodResolved = true;
                                break;
                            }
                        }
                    }

                    if (methodResolved)
                    {
                        break;
                    }

                    if (nativeEng)
                    {
                        asITypeInfo *typeInfo = nativeEng->GetTypeInfoByName(currentType.c_str());

                        if (typeInfo)
                        {
                            for (asUINT m = 0; m < typeInfo->GetMethodCount(); ++m)
                            {
                                asIScriptFunction *methodFunc = typeInfo->GetMethodByIndex(m);

                                if (methodFunc && methodFunc->GetName() == targetTok.text)
                                {
                                    std::string declStr = methodFunc->GetDeclaration(true, true);
                                    size_t tppos = declStr.find("T[]::");

                                    if (tppos != std::string::npos)
                                    {
                                        declStr.erase(tppos, 5);
                                    }

                                    if (declStr.find(cleanTypeName + "::") == std::string::npos)
                                    {
                                        size_t namePos = declStr.find(targetTok.text + "(");

                                        if (namePos != std::string::npos)
                                        {
                                            declStr.insert(namePos, objType + "::");
                                        }
                                    }

                                    hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(declStr));
                                    methodResolved = true;
                                    break;
                                }
                            }
                        }
                    }

                    if (methodResolved)
                    {
                        break;
                    }
                }
            }

            if (hoverResult.empty())
            {
                SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}});
                return;
            }
        }

        if (hoverResult.empty() && (reservedKeywords.count(targetTok.text) || contextualKeywords.count(targetTok.text)) && !primitiveTypes.count(targetTok.text))
        {
            if (targetTok.text != "this" && targetTok.text != "super" && targetTok.text != "function")
            {
                SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}});
                return;
            }
        }

        if (hoverResult.empty() && targetTok.tokenClass == asTC_VALUE)
        {
            if (!targetTok.text.empty() && targetTok.text[0] == '"')
            {
                hoverResult = fmt::format("```cpp\n(const char [{}]) {}\n```", targetTok.text.length() - 1, targetTok.text);
            }
            else
            {
                hoverResult = fmt::format("```cpp\n(int) {}\n```", targetTok.text);
            }
        }

        if (hoverResult.empty() && targetTok.tokenClass == asTC_IDENTIFIER)
        {
            std::string fullQualName = targetTok.text;
            int left = targetIdx - 1;

            while (left >= 0 && allTokens[left].text == "::")
            {
                if (left > 0 && IsWord(allTokens[left - 1].text))
                {
                    fullQualName = allTokens[left - 1].text + "::" + fullQualName;
                    left -= 2;
                }
                else
                {
                    break;
                }
            }

            bool isFollowedByParen = (targetIdx + 1 < (int)allTokens.size() && allTokens[targetIdx + 1].text == "(");
            bool isPrecededByTilde = (targetIdx > 0 && allTokens[targetIdx - 1].text == "~");

            auto MatchesQual = [&](const DeclInfo &decl) -> bool
            {
                if (decl.fullName == fullQualName)
                {
                    return true;
                }

                if (decl.fullName.length() > fullQualName.length())
                {
                    size_t p = decl.fullName.rfind(fullQualName);

                    if (p != std::string::npos && p + fullQualName.length() == decl.fullName.length())
                    {
                        if (p >= 2 && decl.fullName.substr(p - 2, 2) == "::")
                        {
                            return true;
                        }
                    }
                }

                return false;
            };

            for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                const auto &decl = *it;

                if ((decl.type == "local_variable" || decl.type == "parameter" || decl.type == "global_variable") && decl.name == targetTok.text)
                {
                    if (decl.type == "global_variable" || (targetTok.startPos >= decl.startPos && targetTok.startPos <= decl.endPos))
                    {
                        hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(decl.hoverText));
                        break;
                    }
                }
            }

            if (hoverResult.empty() && (isFollowedByParen || isPrecededByTilde || targetTok.text == "function"))
            {
                for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                {
                    const auto &decl = *it;

                    if ((decl.type == "function" || decl.type == "lambda") && MatchesQual(decl))
                    {
                        if (decl.type == "lambda")
                        {
                            if (targetTok.startPos == decl.startPos)
                            {
                                hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(decl.hoverText));
                                break;
                            }
                        }
                        else
                        {
                            hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(decl.hoverText));
                            break;
                        }
                    }
                }
            }

            if (hoverResult.empty())
            {
                for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                {
                    const auto &decl = *it;

                    if (decl.type != "function" && decl.type != "local_variable" && decl.type != "parameter" && decl.type != "global_variable" && decl.type != "lambda" && MatchesQual(decl))
                    {
                        hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(decl.hoverText));
                        break;
                    }
                }
            }

            if (hoverResult.empty() && nativeEng)
            {
                for (asUINT f = 0; f < nativeEng->GetFuncdefCount(); ++f)
                {
                    asITypeInfo *fInfo = nativeEng->GetFuncdefByIndex(f);

                    if (fInfo && fInfo->GetName() == targetTok.text)
                    {
                        asIScriptFunction *fFunc = fInfo->GetFuncdefSignature();

                        if (fFunc)
                        {
                            hoverResult = fmt::format("```cpp\nfuncdef {}\n```", CleanSignature(fFunc->GetDeclaration(true, true)));
                            break;
                        }
                    }
                }

                if (hoverResult.empty())
                {
                    asITypeInfo *tInfo = nativeEng->GetTypeInfoByName(targetTok.text.c_str());

                    if (tInfo)
                    {
                        hoverResult = fmt::format("```cpp\nclass {}\n```", tInfo->GetName());
                    }
                }
            }
        }
    }

    if (!hoverResult.empty())
    {
        json response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"contents", {{"kind", "markdown"}, {"value", hoverResult}}}}}};
        SendToVSCode(response);
    }
    else
    {
        SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}});
    }
}