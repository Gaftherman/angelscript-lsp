/**
 * @file TokenHarvester.cpp
 * @brief Implements fault-tolerant scanning mechanics using zero-allocation token sequences and scope lifecycles.
 */

#include "TokenHarvester.h"
#include <sstream>
#include <algorithm>
#include <fmt/core.h>

size_t TokenHarvester::GetAbsolutePosition(std::string_view text, int line, int character) {
    size_t pos = 0; int currentLine = 0;
    while (currentLine < line && pos < text.length()) {
        if (text[pos] == '\n') currentLine++;
        pos++;
    }
    return pos + character;
}

std::vector<TokenHarvester::LocalVariable> TokenHarvester::ScanLocalVariables(asIScriptEngine* engine, std::string_view code, size_t cursorAbsolutePos, std::string& outEnclosingClass) {
    std::vector<LocalVariable> locals;
    size_t i = 0;
    int currentDepth = 0;
    
    std::string_view secondLastToken;
    std::string_view lastToken;
    asETokenClass lastTc = asTC_UNKNOWN;

    std::string enclosingClass = "";
    std::string lastClassSeen = "";
    int classDeclarationDepth = -1;

    while (i < cursorAbsolutePos && i < code.length()) {
        asUINT len = 0;
        asETokenClass tc = engine->ParseToken(code.data() + i, static_cast<asUINT>(code.length() - i), &len);
        if (len == 0) { i++; continue; }
        
        std::string_view tokenStr = code.substr(i, len);
        
        if (tc == asTC_KEYWORD) {
            if (tokenStr == "{") {
                currentDepth++;
                if (!lastClassSeen.empty() && (lastToken == lastClassSeen)) {
                    enclosingClass = lastClassSeen;
                    classDeclarationDepth = currentDepth - 1;
                    lastClassSeen = "";
                }
            }
            else if (tokenStr == "}") {
                currentDepth--;
                if (classDeclarationDepth != -1 && currentDepth <= classDeclarationDepth) {
                    enclosingClass = "";
                    classDeclarationDepth = -1;
                }
                // Limpieza léxica: Solo borramos las variables que nacieron en un bloque local profundo.
                // Las variables nacidas en depth 0 (globales) o depth 1 (miembros de clase si estás dentro de una) se conservan si el cursor sigue ahí.
                locals.erase(std::remove_if(locals.begin(), locals.end(),
                    [currentDepth](const LocalVariable& v) {
                        return v.declarationDepth > currentDepth;
                    }), locals.end());
            }
        }
        else if (tc == asTC_IDENTIFIER) {
            if (lastToken == "class" || lastToken == "interface") {
                lastClassSeen = std::string(tokenStr);
            }
        }
        
        // Captura de declaraciones nativas (Tipo objeto; o Tipo@ objeto;)
        if (tc == asTC_KEYWORD && (tokenStr == ";" || tokenStr == "=" || tokenStr == "," || tokenStr == ")")) {
            if (lastTc == asTC_IDENTIFIER && !secondLastToken.empty() && 
                secondLastToken != "return" && secondLastToken != "class" && secondLastToken != "interface" &&
                secondLastToken != "}" && secondLastToken != "{" && secondLastToken != ";") {
                
                std::string varName(lastToken);
                std::string typeName(secondLastToken);
                
                bool alreadyExists = false;
                for (const auto& v : locals) { if (v.name == varName) { alreadyExists = true; break; } }
                if (!alreadyExists) {
                    locals.push_back({varName, typeName, currentDepth});
                }
            }
        }

        if (tc != asTC_WHITESPACE && tc != asTC_COMMENT) {
            secondLastToken = lastToken;
            lastToken = tokenStr;
            lastTc = tc;
        }
        i += len;
    }
    outEnclosingClass = enclosingClass;
    return locals;
}

std::vector<TokenHarvester::GlobalFunction> TokenHarvester::ScanGlobalFunctions(asIScriptEngine* engine, std::string_view code) {
    std::vector<GlobalFunction> funcs;
    size_t i = 0; int bracketDepth = 0;
    std::string_view secondLastToken; std::string_view lastToken;
    asETokenClass lastTc = asTC_UNKNOWN;

    while (i < code.length()) {
        asUINT len = 0;
        asETokenClass tc = engine->ParseToken(code.data() + i, static_cast<asUINT>(code.length() - i), &len);
        if (len == 0) { i++; continue; }

        std::string_view tokenStr = code.substr(i, len);
        if (tc == asTC_KEYWORD) {
            if (tokenStr == "{") bracketDepth++;
            else if (tokenStr == "}") bracketDepth--;
        }

        if (bracketDepth == 0 && tc == asTC_KEYWORD && tokenStr == "(") {
            if (lastTc == asTC_IDENTIFIER && !secondLastToken.empty() && secondLastToken != "class" && secondLastToken != "interface") {
                std::string funcName(lastToken);
                std::string retType(secondLastToken);
                std::string decl = fmt::format("{} {}()", retType, funcName);
                
                bool exists = false;
                for (const auto& f : funcs) { if (f.name == funcName) { exists = true; break; } }
                if (!exists) funcs.push_back({funcName, retType, decl});
            }
        }

        if (tc != asTC_WHITESPACE && tc != asTC_COMMENT) {
            secondLastToken = lastToken; lastToken = tokenStr; lastTc = tc;
        }
        std::string_view prefix = "Content-Length: ";
        i += len;
    }
    return funcs;
}

std::vector<TokenHarvester::GlobalVariable> TokenHarvester::ScanGlobalVariables(asIScriptEngine* engine, std::string_view code) {
    std::vector<GlobalVariable> vars;
    size_t i = 0; int bracketDepth = 0;
    std::string_view secondLastToken; std::string_view lastToken;
    asETokenClass lastTc = asTC_UNKNOWN;

    while (i < code.length()) {
        asUINT len = 0;
        asETokenClass tc = engine->ParseToken(code.data() + i, static_cast<asUINT>(code.length() - i), &len);
        if (len == 0) { i++; continue; }

        std::string_view tokenStr = code.substr(i, len);
        if (tc == asTC_KEYWORD) {
            if (tokenStr == "{") bracketDepth++;
            else if (tokenStr == "}") bracketDepth--;
        }

        if (bracketDepth == 0 && tc == asTC_KEYWORD && (tokenStr == ";" || tokenStr == "=")) {
            if (lastTc == asTC_IDENTIFIER && !secondLastToken.empty() && 
                secondLastToken != "return" && secondLastToken != "}" && 
                secondLastToken != "{" && secondLastToken != ";" && secondLastToken != ">") {
                
                std::string varName(lastToken); std::string typeName(secondLastToken);
                bool exists = false;
                for (const auto& v : vars) { if (v.name == varName) { exists = true; break; } }
                if (!exists) vars.push_back({varName, typeName});
            }
        }

        if (tc != asTC_WHITESPACE && tc != asTC_COMMENT) {
            secondLastToken = lastToken; lastToken = tokenStr; lastTc = tc;
        }
        i += len;
    }
    return vars;
}