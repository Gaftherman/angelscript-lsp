/**
 * @file LspServer.cpp
 * @brief Implements highly-optimized JSON-RPC message parsing and routing workflows compliant with LSP lifecycle rules.
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
#include <ranges>
#include <string_view>
#include <cstdio>
#include <charconv>

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
    std::string BlankOutLine(const std::string &text, int line)
    {
        std::string modifiedText = text;
        size_t lineStartPos = 0;
        size_t textLen = text.length();

        for (int i = 0; i < line; ++i)
        {
            lineStartPos = text.find('\n', lineStartPos);
            if (lineStartPos == std::string::npos)
            {
                return modifiedText;
            }
            lineStartPos++;
        }

        size_t lineEndPos = text.find('\n', lineStartPos);
        if (lineEndPos == std::string::npos)
        {
            lineEndPos = textLen;
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

    std::string_view ExtractWordAtPosition(std::string_view text, int line, int character)
    {
        size_t lineStartPos = 0;
        size_t textLen = text.length();

        for (int i = 0; i < line; ++i)
        {
            lineStartPos = text.find('\n', lineStartPos);
            if (lineStartPos == std::string::npos)
            {
                return "";
            }
            lineStartPos++;
        }

        size_t lineEndPos = text.find('\n', lineStartPos);
        if (lineEndPos == std::string::npos)
        {
            lineEndPos = textLen;
        }

        std::string_view lineText = text.substr(lineStartPos, lineEndPos - lineStartPos);
        int start = std::min(character, static_cast<int>(lineText.length()));
        int end = start;

        while (start > 0)
        {
            unsigned char startChar = static_cast<unsigned char>(lineText[start - 1]);
            if (!SAFE_IS_ALNUM(startChar) && lineText[start - 1] != '_')
            {
                break;
            }
            start--;
        }

        while (end < static_cast<int>(lineText.length()))
        {
            unsigned char endChar = static_cast<unsigned char>(lineText[end]);
            if (!SAFE_IS_ALNUM(endChar) && lineText[end] != '_')
            {
                break;
            }
            end++;
        }

        return lineText.substr(start, end - start);
    }

    void PopulateCustomClassesFromModule(asIScriptEngine *engine, asIScriptModule *mod, std::vector<TokenHarvester::ScriptClass> &customClasses)
    {
        if (!mod || !engine)
        {
            return;
        }

        std::unordered_map<std::string, size_t> classIndexMap;
        classIndexMap.reserve(customClasses.size());
        for (size_t i = 0; i < customClasses.size(); ++i)
        {
            classIndexMap[customClasses[i].name] = i;
        }

        asUINT objectTypeCount = mod->GetObjectTypeCount();
        for (asUINT t = 0; t < objectTypeCount; t++)
        {
            asITypeInfo *typeInfo = mod->GetObjectTypeByIndex(t);
            if (!typeInfo)
                continue;

            std::string className = typeInfo->GetName();
            auto it = classIndexMap.find(className);

            if (it == classIndexMap.end())
            {
                TokenHarvester::ScriptClass extClass;
                extClass.name = className;

                asUINT propertyCount = typeInfo->GetPropertyCount();
                for (asUINT p = 0; p < propertyCount; p++)
                {
                    const char *pName = nullptr;
                    int pTypeId = 0;
                    typeInfo->GetProperty(p, &pName, &pTypeId);
                    const char *pDecl = engine->GetTypeDeclaration(pTypeId, true);

                    if (pName && pDecl)
                    {
                        extClass.properties[pName] = TokenHarvester::ClassProperty{pName, pDecl, "public"};
                    }
                }

                asUINT methodCount = typeInfo->GetMethodCount();
                for (asUINT m = 0; m < methodCount; m++)
                {
                    asIScriptFunction *method = typeInfo->GetMethodByIndex(m);
                    if (!method)
                        continue;

                    const char *rDecl = engine->GetTypeDeclaration(method->GetReturnTypeId(), true);
                    std::string mName = method->GetName();
                    bool isConstructorOrDestructor = (mName == className || mName == ("~" + className));

                    extClass.methods[mName].push_back(TokenHarvester::ClassMethod{
                        mName,
                        rDecl ? rDecl : "void",
                        method->GetDeclaration(true, false, true),
                        "public",
                        isConstructorOrDestructor});
                }

                customClasses.push_back(std::move(extClass));
                classIndexMap[className] = customClasses.size() - 1;
            }
        }

        asUINT enumCount = mod->GetEnumCount();
        for (asUINT e = 0; e < enumCount; e++)
        {
            asITypeInfo *enumType = mod->GetEnumByIndex(e);
            if (!enumType)
                continue;

            std::string enumName = enumType->GetName();
            auto it = classIndexMap.find(enumName);
            asUINT enumValueCount = enumType->GetEnumValueCount();

            if (it != classIndexMap.end())
            {
                auto &existingMap = customClasses[it->second];
                existingMap.properties.clear();
                existingMap.methods.clear();

                for (asUINT v = 0; v < enumValueCount; v++)
                {
                    asINT64 val = 0;
                    if (const char *enumValName = enumType->GetEnumValueByIndex(v, &val))
                    {
                        existingMap.properties[enumValName] = TokenHarvester::ClassProperty{enumValName, enumName, "public"};
                    }
                }
            }
            else
            {
                TokenHarvester::ScriptClass extEnum;
                extEnum.name = enumName;

                for (asUINT v = 0; v < enumValueCount; v++)
                {
                    asINT64 val = 0;
                    if (const char *enumValName = enumType->GetEnumValueByIndex(v, &val))
                    {
                        extEnum.properties[enumValName] = TokenHarvester::ClassProperty{enumValName, enumName, "public"};
                    }
                }

                customClasses.push_back(std::move(extEnum));
                classIndexMap[enumName] = customClasses.size() - 1;
            }
        }
    }

    bool IsCustomScriptType(asIScriptModule *mod, const std::string &name)
    {
        if (!mod)
            return false;

        asUINT objectTypeCount = mod->GetObjectTypeCount();
        for (asUINT t = 0; t < objectTypeCount; t++)
        {
            if (asITypeInfo *ti = mod->GetObjectTypeByIndex(t); ti && ti->GetName() == name)
            {
                return true;
            }
        }

        asUINT enumCount = mod->GetEnumCount();
        for (asUINT e = 0; e < enumCount; e++)
        {
            if (asITypeInfo *ti = mod->GetEnumByIndex(e); ti && ti->GetName() == name)
            {
                return true;
            }
        }
        return false;
    }

    bool IsEngineOrScriptType(asIScriptEngine *engine, asIScriptModule *mod, const std::string &name)
    {
        if (engine && engine->GetTypeIdByDecl(name.c_str()) >= 0)
        {
            return true;
        }
        return IsCustomScriptType(mod, name);
    }

    bool IsLspNotification(std::string_view method) noexcept
    {
        return method == LspConstants::MethodDidOpen || method == LspConstants::MethodDidChange;
    }

    bool IsIncludeDirective(std::string_view line, size_t &outPathStart, size_t &outPathEnd) noexcept
    {
        size_t firstChar = line.find_first_not_of(" \t\r\n");
        if (firstChar == std::string::npos || line[firstChar] != '#')
        {
            return false;
        }

        size_t includePos = line.find(LspConstants::IncludeKeyword, firstChar + 1);
        if (includePos == std::string::npos)
        {
            return false;
        }

        for (size_t check = firstChar + 1; check < includePos; ++check)
        {
            if (line[check] != ' ' && line[check] != '\t')
                return false;
        }

        size_t startDelim = line.find_first_of("\"<", includePos + LspConstants::IncludeKeyword.length());
        if (startDelim == std::string::npos)
        {
            return false;
        }

        char closeChar = (line[startDelim] == '"') ? '"' : '>';
        size_t endDelim = line.find(closeChar, startDelim + 1);
        if (endDelim == std::string::npos)
        {
            return false;
        }

        outPathStart = startDelim + 1;
        outPathEnd = endDelim;
        return true;
    }
}

void AngelScriptLSPServer::Run()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::string line;
    std::string content;
    constexpr std::string_view prefix = LspConstants::ContentLengthPrefix;

    while (true)
    {
        size_t contentLength = 0;

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

            if (line.starts_with(prefix))
            {
                std::string_view lengthView = std::string_view(line).substr(prefix.length());
                auto [ptr, ec] = std::from_chars(lengthView.data(), lengthView.data() + lengthView.size(), contentLength);
                if (ec != std::errc{})
                {
                    contentLength = 0;
                }
            }
        }

        if (std::cin.eof() || std::cin.fail())
        {
            break;
        }

        if (contentLength == 0)
        {
            continue;
        }

        content.resize(contentLength);
        std::cin.read(content.data(), contentLength);

        if (std::cin.gcount() < static_cast<std::streamsize>(contentLength))
        {
            break;
        }

        try
        {
            json request = json::parse(content);
            if (!request.contains("method"))
                continue;

            std::string method = request["method"].get<std::string>();

            if (method == LspConstants::MethodInitialize)
            {
                HandleInitialize(request["id"]);
            }
            else if (LspServer::IsLspNotification(method))
            {
                std::string uri = request["params"]["textDocument"]["uri"].get<std::string>();
                std::string text = (method == LspConstants::MethodDidOpen)
                                       ? request["params"]["textDocument"]["text"].get<std::string>()
                                       : request["params"]["contentChanges"][0]["text"].get<std::string>();

                documentCache[uri] = text;
                AnalyzeAndReport(uri, text);
            }
            else if (method == LspConstants::MethodSemanticTokens)
            {
                std::string uri = request["params"]["textDocument"]["uri"].get<std::string>();
                if (documentCache.contains(uri))
                {
                    HandleSemanticTokens(request["id"], uri);
                }
            }
            else if (method == LspConstants::MethodCompletion)
            {
                std::string uri = request["params"]["textDocument"]["uri"].get<std::string>();
                if (documentCache.contains(uri))
                {
                    HandleCompletion(request["id"], uri, request["params"]["position"]["line"].get<int>(), request["params"]["position"]["character"].get<int>());
                }
            }
            else if (method == LspConstants::MethodHover)
            {
                std::string uri = request["params"]["textDocument"]["uri"].get<std::string>();
                if (documentCache.contains(uri))
                {
                    HandleHover(request["id"], uri, request["params"]["position"]["line"].get<int>(), request["params"]["position"]["character"].get<int>());
                }
            }
        }
        catch (...)
        {
        }
    }
}

void AngelScriptLSPServer::SendToVSCode(const json &message)
{
    std::string content = message.dump();
    fmt::print("Content-Length: {}\r\n\r\n{}", content.length(), content);
    std::fflush(stdout);
}

void AngelScriptLSPServer::LogRemote(std::string_view message, int logType)
{
    SendToVSCode({{"jsonrpc", "2.0"},
                  {"method", "window/logMessage"},
                  {"params", {{"type", logType}, {"message", std::string(message)}}}});
}

std::string AngelScriptLSPServer::TransformUriToPath(std::string_view uri)
{
    std::string_view remaining = uri;
    if (remaining.starts_with(LspConstants::UriFilePrefixWin))
    {
        remaining.remove_prefix(LspConstants::UriFilePrefixWin.length());
    }
    else if (remaining.starts_with(LspConstants::UriFilePrefixUnix))
    {
        remaining.remove_prefix(LspConstants::UriFilePrefixUnix.length());
    }

    std::string decoded;
    decoded.reserve(remaining.length());

    for (size_t i = 0; i < remaining.length(); ++i)
    {
        if (remaining[i] == '%' && i + 2 < remaining.length())
        {
            char hexChar1 = remaining[i + 1];
            char hexChar2 = remaining[i + 2];

            int highNibble = (hexChar1 >= '0' && hexChar1 <= '9') ? (hexChar1 - '0') : (hexChar1 >= 'a' && hexChar1 <= 'f') ? (hexChar1 - 'a' + 10)
                                                                                   : (hexChar1 >= 'A' && hexChar1 <= 'F')   ? (hexChar1 - 'A' + 10)
                                                                                                                            : -1;
            int lowNibble = (hexChar2 >= '0' && hexChar2 <= '9') ? (hexChar2 - '0') : (hexChar2 >= 'a' && hexChar2 <= 'f') ? (hexChar2 - 'a' + 10)
                                                                                  : (hexChar2 >= 'A' && hexChar2 <= 'F')   ? (hexChar2 - 'A' + 10)
                                                                                                                           : -1;

            if (highNibble != -1 && lowNibble != -1)
            {
                decoded += static_cast<char>((highNibble << 4) | lowNibble);
                i += 2;
                continue;
            }
        }
        decoded += remaining[i];
    }
    return decoded;
}

void AngelScriptLSPServer::HandleInitialize(json id)
{
    SendToVSCode({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result", {{"capabilities", {{"textDocumentSync", 1}, {"semanticTokensProvider", {{"legend", {{"tokenTypes", std::vector<std::string>{"keyword", "type", "function", "variable", "number", "string", "comment", "operator"}}, {"tokenModifiers", std::vector<std::string>()}}}, {"full", true}}}, {"completionProvider", {{"resolveProvider", false}, {"triggerCharacters", std::vector<std::string>{".", ":", "@"}}}}, {"hoverProvider", true}}}}}});
    LogRemote("AngelScript LSP engine worker module successfully attached.", 3);
}

void AngelScriptLSPServer::AnalyzeAndReport(const std::string &uri, const std::string &code)
{
    std::string cleanUri = TransformUriToPath(uri);
    fs::path currentPath(cleanUri);
    fs::path baseDir = currentPath.parent_path();
    json diagnosticsArray = json::array();

    scriptEngine.ClearDiagnostics();
    scriptEngine.BuildModule("LSPModule", cleanUri, code);

    for (const auto &diag : scriptEngine.GetDiagnostics())
    {
        int line = std::max(0, diag.row - 1);
        int col = std::max(0, diag.col - 1);

        diagnosticsArray.push_back({{"range", {{"start", {{"line", line}, {"character", col}}}, {"end", {{"line", line}, {"character", col + 5}}}}},
                                    {"severity", (diag.type == asMSGTYPE_WARNING) ? 2 : 1},
                                    {"message", diag.message},
                                    {"source", std::string(LspConstants::DiagnosticSource)}});
    }

    std::string_view remainingCode = code;
    size_t lineStartPos = 0;
    int lineIndex = 0;

    while (lineStartPos < remainingCode.length())
    {
        size_t lineEndPos = remainingCode.find('\n', lineStartPos);
        if (lineEndPos == std::string_view::npos)
            lineEndPos = remainingCode.length();

        std::string_view lineStr = remainingCode.substr(lineStartPos, lineEndPos - lineStartPos);
        size_t pathStart = 0, pathEnd = 0;

        if (LspServer::IsIncludeDirective(lineStr, pathStart, pathEnd))
        {
            std::string includeFileName = std::string(lineStr.substr(pathStart, pathEnd - pathStart));
            if (!fs::exists(baseDir / fs::path(includeFileName)))
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

void AngelScriptLSPServer::HandleSemanticTokens(json id, const std::string &uri)
{
    std::string_view code = documentCache[uri];
    std::vector<int> tokens;
    int prevLine = 0, prevChar = 0;
    int currentLine = 0, currentChar = 0;
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
            std::string textStr = std::string(code.substr(i, len));
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
                currentChar++;
        }
        i += (len == 0 ? 1 : len);
    }
    SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"data", tokens}}}});
}

void AngelScriptLSPServer::HandleCompletion(json id, const std::string &uri, int line, int character)
{
    std::string originalText = documentCache[uri];
    asIScriptEngine *nativeEng = scriptEngine.GetNativeEngine();
    size_t cursorAbsPos = TokenHarvester::GetAbsolutePosition(originalText, line, character);
    TokenHarvester::CompletionContext ctx = TokenHarvester::GetCompletionContext(nativeEng, originalText, cursorAbsPos);
    std::string modifiedText = LspServer::BlankOutLine(originalText, line);
    std::string cleanUri = TransformUriToPath(uri);

    scriptEngine.BuildModule("LSPModule", cleanUri, modifiedText);
    asIScriptModule *mod = nativeEng->GetModule("LSPModule");
    std::vector<TokenHarvester::ScriptClass> customClasses = TokenHarvester::ScanCustomClasses(nativeEng, originalText);

    LspServer::PopulateCustomClassesFromModule(nativeEng, mod, customClasses);

    std::vector<TokenHarvester::GlobalFunction> tokenFuncs = TokenHarvester::ScanGlobalFunctions(nativeEng, originalText);
    std::vector<TokenHarvester::GlobalVariable> tokenGlobalVars = TokenHarvester::ScanGlobalVariables(nativeEng, originalText);
    std::string enclosingClass;
    std::vector<TokenHarvester::LocalVariable> localVars = TokenHarvester::ScanLocalVariables(nativeEng, originalText, cursorAbsPos, enclosingClass, customClasses, tokenGlobalVars, tokenFuncs);

    std::function<void(const std::string &)> logger = [](const std::string &msg)
    {
        std::cerr << "[AST Debug] " << msg << std::endl;
    };

    CompletionHandler handler(nativeEng, mod, ctx, enclosingClass, localVars, customClasses, tokenFuncs, tokenGlobalVars, logger);
    json itemsArray = handler.GenerateItems(originalText, cursorAbsPos);

    SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isIncomplete", false}, {"items", itemsArray}}}});
}

void AngelScriptLSPServer::HandleHover(json id, const std::string &uri, int line, int character)
{
    std::string originalText = documentCache[uri];
    asIScriptEngine *nativeEng = scriptEngine.GetNativeEngine();

    HoverHandler handler(nativeEng, originalText, line, character);
    json hoverResult = handler.Process();

    SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", hoverResult}});
}