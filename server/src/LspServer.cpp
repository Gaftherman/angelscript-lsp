/**
 * @file LspServer.cpp
 * @brief Implements JSON-RPC message parsing and routing workflows compliant with LSP lifecycle rules.
 * @author AngelScript LSP Team
 */

#include "LspServer.h"
#include "TokenHarvester.h"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <fmt/core.h>

// CRITICAL FIX: Explicit Win32 binary stream headers to override carriage return text-mode translations
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace fs = std::filesystem;

void AngelScriptLSPServer::Run() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    while (true) {
        int contentLength = 0; std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;

            std::string_view prefix = "Content-Length: ";
            if (line.compare(0, prefix.length(), prefix) == 0) {
                contentLength = std::stoi(std::string(line.substr(prefix.length())));
            }
        }
        if (contentLength == 0) continue;

        std::string content(contentLength, ' ');
        std::cin.read(&content[0], contentLength);

        try {
            json request = json::parse(content);
            if (request.contains("method")) {
                std::string method = request["method"];

                if (method == "initialize") HandleInitialize(request["id"]);
                else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
                    std::string uri = request["params"]["textDocument"]["uri"];
                    std::string text = (method == "textDocument/didOpen") 
                        ? request["params"]["textDocument"]["text"] 
                        : request["params"]["contentChanges"][0]["text"];
                    documentCache[uri] = text; 
                    AnalyzeAndReport(uri, text);
                }
                else if (method == "textDocument/semanticTokens/full") {
                    std::string uri = request["params"]["textDocument"]["uri"];
                    if (documentCache.find(uri) != documentCache.end()) HandleSemanticTokens(request["id"], uri);
                }
                else if (method == "textDocument/completion") {
                    json id = request["id"]; std::string uri = request["params"]["textDocument"]["uri"];
                    int l = request["params"]["position"]["line"]; int c = request["params"]["position"]["character"];
                    if (documentCache.find(uri) != documentCache.end()) HandleCompletion(id, uri, l, c);
                }
                else if (method == "textDocument/hover") {
                    json id = request["id"]; std::string uri = request["params"]["textDocument"]["uri"];
                    int l = request["params"]["position"]["line"]; int c = request["params"]["position"]["character"];
                    if (documentCache.find(uri) != documentCache.end()) HandleHover(id, uri, l, c);
                }
            }
        } catch (...) {}
    }
}

void AngelScriptLSPServer::SendToVSCode(const json& message) {
    std::string content = message.dump();
    fmt::print("Content-Length: {}\r\n\r\n{}", content.length(), content);
    std::cout << std::flush;
}

void AngelScriptLSPServer::LogRemote(std::string_view message, int logType) {
    json logNotification = {
        {"jsonrpc", "2.0"}, {"method", "window/logMessage"},
        {"params", {{"type", logType}, {"message", std::string(message)}}}
    };
    SendToVSCode(logNotification);
}

std::string AngelScriptLSPServer::TransformUriToPath(std::string_view uri) {
    std::string ret(uri);
    if (ret.find("file:///") == 0) {
        #ifdef _WIN32
        ret = ret.substr(8);
        #else
        ret = ret.substr(7);
        #endif
    }
    std::string decoded; decoded.reserve(ret.length());
    for (size_t i = 0; i < ret.length(); ++i) {
        if (ret[i] == '%' && i + 2 < ret.length()) {
            int hex; std::istringstream hexStream(ret.substr(i + 1, 2)); hexStream >> std::hex >> hex;
            decoded += static_cast<char>(hex); i += 2;
        } else decoded += ret[i];
    }
    return decoded;
}

void AngelScriptLSPServer::HandleInitialize(json id) {
    json response = {
        {"jsonrpc", "2.0"}, {"id", id},
        {"result", { {"capabilities", { 
            {"textDocumentSync", 1},
            {"semanticTokensProvider", {
                {"legend", {
                    {"tokenTypes", std::vector<std::string>{"keyword", "type", "function", "variable", "number", "string", "comment", "operator"}},
                    {"tokenModifiers", std::vector<std::string>()}
                }},
                {"full", true}
            }},
            {"completionProvider", { {"resolveProvider", false}, {"triggerCharacters", std::vector<std::string>{".", ":", "@"}} }},
            {"hoverProvider", true}
        }} }}
    };
    SendToVSCode(response);
    LogRemote("AngelScript LSP engine worker module successfully attached.", 3);
}

void AngelScriptLSPServer::AnalyzeAndReport(const std::string& uri, const std::string& code) {
    scriptEngine.ClearDiagnostics();
    std::string cleanUri = TransformUriToPath(uri);
    scriptEngine.BuildModule("LSPModule", cleanUri, code);

    json diagnosticsArray = json::array();
    for (const auto& diag : scriptEngine.GetDiagnostics()) {
        int line = std::max(0, diag.row - 1); int col = std::max(0, diag.col - 1);
        diagnosticsArray.push_back({
            {"range", {{"start", {{"line", line}, {"character", col}}}, {"end", {{"line", line}, {"character", col + 5}}}}},
            {"severity", (diag.type == asMSGTYPE_WARNING) ? 2 : 1}, {"message", diag.message}, {"source", "AngelScript"}
        });
    }
    SendToVSCode({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", {{"uri", uri}, {"diagnostics", diagnosticsArray}}}});
}

void AngelScriptLSPServer::HandleSemanticTokens(json id, const std::string& uri) {
    std::string_view code = documentCache[uri];
    std::vector<int> tokens;
    int prevLine = 0, prevChar = 0, currentLine = 0, currentChar = 0;
    size_t i = 0;
    
    asIScriptEngine* nativeEng = scriptEngine.GetNativeEngine();
    while (i < code.length()) {
        asUINT len = 0;
        asETokenClass tc = nativeEng->ParseToken(code.data() + i, static_cast<asUINT>(code.length() - i), &len);
        int tokenType = -1; 
        
        if (tc == asTC_KEYWORD) tokenType = 0; 
        else if (tc == asTC_VALUE) tokenType = (code[i] == '"' || code[i] == '\'') ? 5 : 4; 
        else if (tc == asTC_COMMENT) tokenType = 6; 
        else if (tc == asTC_IDENTIFIER) {
            std::string_view text = code.substr(i, len);
            if (text == "string" || text == "array" || text == "dictionary" || text == "int" || text == "float" || text == "bool" || text == "void" || text == "uint") {
                tokenType = 1; 
            } else {
                tokenType = 3; size_t nextPos = i + len;
                // CRITICAL FIX: Safe character cast for internal verification loop
                while (nextPos < code.length() && isspace(static_cast<unsigned char>(code[nextPos]))) nextPos++;
                if (nextPos < code.length() && code[nextPos] == '(') tokenType = 2; 
            }
        }

        if (tokenType != -1) {
            int deltaLine = currentLine - prevLine;
            int deltaChar = (deltaLine == 0) ? (currentChar - prevChar) : currentChar;
            tokens.push_back(deltaLine); tokens.push_back(deltaChar); tokens.push_back(len);
            tokens.push_back(tokenType); tokens.push_back(0); 
            prevLine = currentLine; prevChar = currentChar;
        }

        for (asUINT j = 0; j < len; ++j) {
            if (code[i + j] == '\n') { currentLine++; currentChar = 0; } else currentChar++;
        }
        i += (len == 0 ? 1 : len);
    }
    SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", { {"data", tokens} }}});
}

void AngelScriptLSPServer::HandleCompletion(json id, const std::string& uri, int line, int character) {
    std::string originalText = documentCache[uri];
    asIScriptEngine* nativeEng = scriptEngine.GetNativeEngine();
    size_t cursorAbsPos = TokenHarvester::GetAbsolutePosition(originalText, line, character);
    
    std::vector<std::string> lines;
    std::istringstream stream(originalText); std::string tmpLine;
    while (std::getline(stream, tmpLine)) {
        if (!tmpLine.empty() && tmpLine.back() == '\r') tmpLine.pop_back();
        lines.push_back(tmpLine);
    }

    json itemsArray = json::array();
    if (line >= (int)lines.size()) {
        SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isIncomplete", false}, {"items", itemsArray}}}}); 
        return;
    }

    std::string currentLineText = lines[line];
    
    // CRITICAL FIX 1: Look behind the current typing context word layout safely via unsigned char casting
    int pos = character - 1;
    if (pos >= (int)currentLineText.length()) pos = (int)currentLineText.length() - 1;

    while (pos >= 0 && isspace(static_cast<unsigned char>(currentLineText[pos]))) pos--;
    
    // Clear the active word the user is typing to check what context symbol preceded it
    int endWordPos = pos;
    while (endWordPos >= 0 && (isalnum(static_cast<unsigned char>(currentLineText[endWordPos])) || currentLineText[endWordPos] == '_')) {
        endWordPos--;
    }
    while (endWordPos >= 0 && isspace(static_cast<unsigned char>(currentLineText[endWordPos]))) {
        endWordPos--;
    }

    // CRITICAL FIX 2: If statement composition reveals a dangling handle symbol '@' left of the user expression word, suppress global autocomplete
    if (pos >= 0 && (currentLineText[pos] == '@' || (endWordPos >= 0 && currentLineText[endWordPos] == '@'))) {
        SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isIncomplete", false}, {"items", itemsArray}}}});
        return;
    }

    bool isDotCompletion = false; std::string objectName = "";
    if (pos >= 0 && currentLineText[pos] == '.') {
        isDotCompletion = true; pos--;
        while (pos >= 0 && isspace(static_cast<unsigned char>(currentLineText[pos]))) pos--;
        int endObj = pos;
        while (pos >= 0 && (isalnum(static_cast<unsigned char>(currentLineText[pos])) || currentLineText[pos] == '_')) pos--;
        objectName = currentLineText.substr(pos + 1, endObj - pos);
    }

    // Line sanitization loop
    std::string modifiedText = originalText;
    size_t lineStartPos = 0;
    for (int i = 0; i < line; ++i) {
        lineStartPos = originalText.find('\n', lineStartPos);
        if (lineStartPos == std::string::npos) break;
        lineStartPos++;
    }
    if (lineStartPos != std::string::npos) {
        size_t lineEndPos = originalText.find('\n', lineStartPos);
        if (lineEndPos == std::string::npos) lineEndPos = originalText.length();
        for (size_t p = lineStartPos; p < lineEndPos; ++p) {
            if (modifiedText[p] != '\r' && modifiedText[p] != '\n') modifiedText[p] = ' ';
        }
    }

    std::string cleanUri = TransformUriToPath(uri);
    scriptEngine.BuildModule("LSPModule", cleanUri, modifiedText); 
    asIScriptModule* mod = nativeEng->GetModule("LSPModule");

    std::string enclosingClass = "";
    auto localVars = TokenHarvester::ScanLocalVariables(nativeEng, originalText, cursorAbsPos, enclosingClass);
    auto tokenFuncs = TokenHarvester::ScanGlobalFunctions(nativeEng, originalText);
    auto tokenGlobalVars = TokenHarvester::ScanGlobalVariables(nativeEng, originalText);

    if (isDotCompletion && !objectName.empty()) {
        std::string inferredTypeName = "";
        for (const auto& v : localVars) { if (v.name == objectName) { inferredTypeName = v.typeName; break; } }
        if (inferredTypeName.empty()) {
            std::regex typeRegex(R"(\b([A-Za-z_]\w*)\s+(?:@\s*)?)" + objectName + R"(\b)");
            std::smatch match;
            if (std::regex_search(originalText, match, typeRegex) && match.size() > 1) inferredTypeName = match[1].str();
        }

        if (!inferredTypeName.empty() && mod) {
            asITypeInfo* targetType = mod->GetTypeInfoByName(inferredTypeName.c_str());
            if (!targetType) targetType = nativeEng->GetTypeInfoByName(inferredTypeName.c_str());

            if (targetType) {
                for (asUINT p = 0; p < targetType->GetPropertyCount(); p++) {
                    const char* propName = nullptr; int propTypeId = 0;
                    targetType->GetProperty(p, &propName, &propTypeId);
                    asITypeInfo* propType = nativeEng->GetTypeInfoById(propTypeId);
                    itemsArray.push_back({{"label", propName}, {"kind", 5}, {"detail", propType ? propType->GetName() : "primitive"}});
                }
                for (asUINT m = 0; m < targetType->GetMethodCount(); m++) {
                    asIScriptFunction* func = targetType->GetMethodByIndex(m);
                    itemsArray.push_back({
                        {"label", func->GetName()}, {"kind", 2}, 
                        {"detail", func->GetDeclaration(true, false, true)},
                        {"insertText", std::string(func->GetName()) + "($1)"}, {"insertTextFormat", 2}
                    });
                }
            }
        }
    } 
    else {
        std::vector<std::string> keywords = {"class", "interface", "void", "int", "float", "string", "array", "bool", "if", "else", "for", "while", "return", "this"};
        for (const auto& kw : keywords) itemsArray.push_back({{"label", kw}, {"kind", 14}, {"detail", "keyword"}});
        for (const auto& v : localVars) itemsArray.push_back({{"label", v.name}, {"kind", 6}, {"detail", "local " + v.typeName}});
        
        if (!enclosingClass.empty() && mod) {
            asITypeInfo* classType = mod->GetTypeInfoByName(enclosingClass.c_str());
            if (!classType) classType = nativeEng->GetTypeInfoByName(enclosingClass.c_str());
            
            if (classType) {
                for (asUINT p = 0; p < classType->GetPropertyCount(); p++) {
                    const char* propName = nullptr; int propTypeId = 0;
                    classType->GetProperty(p, &propName, &propTypeId);
                    asITypeInfo* propType = nativeEng->GetTypeInfoById(propTypeId);
                    itemsArray.push_back({
                        {"label", propName}, {"kind", 5}, 
                        {"detail", propType ? propType->GetName() : "primitive"},
                        {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member property of enclosing class `{}`", enclosingClass)}}}
                    });
                }
                for (asUINT m = 0; m < classType->GetMethodCount(); m++) {
                    asIScriptFunction* func = classType->GetMethodByIndex(m);
                    if (func) {
                        std::string funcName = func->GetName();
                        itemsArray.push_back({
                            {"label", funcName}, {"kind", 2}, 
                            {"detail", func->GetDeclaration(true, false, true)},
                            {"documentation", {{"kind", "markdown"}, {"value", fmt::format("Member method of enclosing class `{}`", enclosingClass)}}},
                            {"insertText", funcName + "($1)"}, {"insertTextFormat", 2}
                        });
                    }
                }
            }
        }

        std::unordered_map<std::string, bool> addedFunctions;

        if (mod) {
            for (asUINT f = 0; f < mod->GetFunctionCount(); f++) {
                asIScriptFunction* func = mod->GetFunctionByIndex(f);
                if (func) {
                    std::string funcName = func->GetName(); addedFunctions[funcName] = true;
                    const char* section = nullptr; int r = 0, c = 0;
                    func->GetDeclaredAt(&section, &r, &c);
                    std::string filename = section ? fs::path(section).filename().string() : "workspace";

                    itemsArray.push_back({
                        {"label", funcName}, {"kind", 3}, 
                        {"detail", func->GetDeclaration(true, false, true)},
                        {"documentation", {{"kind", "markdown"}, {"value", fmt::format("**File:** {}", filename)}}},
                        {"insertText", funcName + "($1)"}, {"insertTextFormat", 2}
                    });
                }
            }
            for (asUINT g = 0; g < mod->GetGlobalVarCount(); g++) {
                const char* varName = nullptr; int typeId = 0;
                mod->GetGlobalVar(g, &varName, nullptr, &typeId);
                if (varName) {
                    itemsArray.push_back({
                        {"label", varName}, {"kind", 6}, {"detail", "Global variable"},
                        {"documentation", {{"kind", "markdown"}, {"value", "Global var in module"}}}
                    });
                }
            }
        }

        for (const auto& tf : tokenFuncs) {
            if (!addedFunctions[tf.name]) {
                addedFunctions[tf.name] = true;
                std::string docLabel = fmt::format("**File:** {}", fs::path(cleanUri).filename().string());
                itemsArray.push_back({
                    {"label", tf.name}, {"kind", 3}, {"detail", tf.declaration},
                    {"documentation", {{"kind", "markdown"}, {"value", docLabel}}},
                    {"insertText", tf.name + "($1)"}, {"insertTextFormat", 2}
                });
            }
        }

        for (const auto& nativeVar : tokenGlobalVars) {
            bool exists = false;
            for (const auto& item : itemsArray) { if (item.contains("label") && item["label"] == nativeVar.name) { exists = true; break; } }
            if (!exists) {
                itemsArray.push_back({
                    {"label", nativeVar.name}, {"kind", 6}, {"detail", nativeVar.typeName + " " + nativeVar.name},
                    {"documentation", {{"kind", "markdown"}, {"value", "Variable (parsed fallback)"}}}
                });
            }
        }

        for (asUINT f = 0; f < nativeEng->GetGlobalFunctionCount(); f++) {
            asIScriptFunction* func = nativeEng->GetGlobalFunctionByIndex(f);
            if (func) {
                std::string funcName = func->GetName();
                if (!addedFunctions[funcName]) {
                    itemsArray.push_back({
                        {"label", funcName}, {"kind", 3}, 
                        {"detail", func->GetDeclaration(true, false, true)},
                        {"documentation", {{"kind", "markdown"}, {"value", "**Native Global Function**"}}},
                        {"insertText", funcName + "($1)"}, {"insertTextFormat", 2}
                    });
                }
            }
        }

        for (asUINT g = 0; g < nativeEng->GetGlobalPropertyCount(); g++) {
            const char* varName = nullptr;
            nativeEng->GetGlobalPropertyByIndex(g, &varName, nullptr, nullptr, nullptr);
            if (varName) {
                itemsArray.push_back({
                    {"label", varName}, {"kind", 6}, {"detail", "Native global variable"},
                    {"documentation", {{"kind", "markdown"}, {"value", "Native engine property"}}}
                });
            }
        }
    }

    json response = {
        {"jsonrpc", "2.0"}, {"id", id},
        {"result", {{"isIncomplete", false}, {"items", itemsArray}}}
    };
    SendToVSCode(response);
}

void AngelScriptLSPServer::HandleHover(json id, const std::string& uri, int line, int character) {
    std::string_view text = documentCache[uri];
    asIScriptEngine* nativeEng = scriptEngine.GetNativeEngine();

    std::string codeCopy(text);
    std::istringstream stream(codeCopy);
    std::string currentLineText;
    for (int i = 0; i <= line; ++i) {
        if (!std::getline(stream, currentLineText)) break;
    }

    int start = character; 
    if (start > (int)currentLineText.length()) start = (int)currentLineText.length();
    int end = start;

    while (start > 0 && (isalnum(static_cast<unsigned char>(currentLineText[start - 1])) || currentLineText[start - 1] == '_')) start--;
    while (end < (int)currentLineText.length() && (isalnum(static_cast<unsigned char>(currentLineText[end])) || currentLineText[end] == '_')) end++;
    
    std::string word = currentLineText.substr(start, end - start);
    if (word.empty()) { SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}}); return; }

    std::string hoverResult = "";
    size_t cursorAbsPos = TokenHarvester::GetAbsolutePosition(text, line, character);
    std::string dummyEnclosingClass = "";
    auto localVars = TokenHarvester::ScanLocalVariables(nativeEng, text, cursorAbsPos, dummyEnclosingClass);
    for (const auto& v : localVars) {
        if (v.name == word) {
            hoverResult = fmt::format("Local variable: `{} {}`", v.typeName, v.name);
            break;
        }
    }

    if (hoverResult.empty()) {
        asITypeInfo* typeInfo = nativeEng->GetTypeInfoByName(word.c_str());
        if (typeInfo) {
            hoverResult = fmt::format("```cpp\nclass {}\n```\nRegistered object layout definition.", typeInfo->GetName());
        }
    }

    if (hoverResult.empty()) {
        for (asUINT f = 0; f < nativeEng->GetGlobalFunctionCount(); f++) {
            asIScriptFunction* func = nativeEng->GetGlobalFunctionByIndex(f);
            if (std::string(func->GetName()) == word) {
                hoverResult = fmt::format("```cpp\n{}\n```", func->GetDeclaration(false, false, true));
                break;
            }
        }
    }

    if (!hoverResult.empty()) {
        json response = {
            {"jsonrpc", "2.0"}, {"id", id},
            {"result", {{"contents", {{"kind", "markdown"}, {"value", hoverResult}}}}}
        };
        SendToVSCode(response);
    } else {
        SendToVSCode({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}});
    }
}