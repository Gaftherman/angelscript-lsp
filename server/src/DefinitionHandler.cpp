/**
 * @file DefinitionHandler.cpp
 * @brief Implements full semantic validation tracing for definitions using the hover extraction pipeline.
 */

#include "DefinitionHandler.h"
#include <algorithm>
#include <cctype>

namespace
{
    constexpr std::string_view KEY_PARAMS = "params";
    constexpr std::string_view KEY_TEXT_DOCUMENT = "textDocument";
    constexpr std::string_view KEY_URI = "uri";
    constexpr std::string_view KEY_POSITION = "position";
    constexpr std::string_view KEY_LINE = "line";
    constexpr std::string_view KEY_CHARACTER = "character";
}

namespace DefinitionUtils
{
    const std::unordered_set<std::string_view> DefinitionResolver::reservedKeywords = {"and", "auto", "bool", "break", "case", "cast", "catch", "class", "const", "continue", "default", "do", "double", "else", "enum", "false", "float", "for", "foreach", "funcdef", "if", "import", "in", "inout", "int", "interface", "int8", "int16", "int32", "int64", "is", "mixin", "namespace", "not", "null", "or", "out", "private", "protected", "return", "switch", "true", "try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64", "using", "void", "while", "xor"};
    const std::unordered_set<std::string_view> DefinitionResolver::contextualKeywords = {"abstract", "delete", "explicit", "external", "final", "from", "function", "get", "override", "property", "set", "shared", "super", "this"};
    const std::unordered_set<std::string_view> DefinitionResolver::primitiveTypes = {"void", "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64", "float", "double", "bool", "auto"};
    const std::unordered_set<std::string_view> DefinitionResolver::storageModifiers = {"private", "protected", "public", "shared", "external"};
    const std::unordered_set<std::string_view> DefinitionResolver::structureKeywords = {"class", "interface", "namespace", "enum", "mixin", "abstract"};
    const std::unordered_set<std::string_view> DefinitionResolver::statementKeywords = {"if", "for", "foreach", "while", "return", "break", "continue", "switch", "case", "default", "cast", "try", "catch", "delete", "throw"};

    DefinitionResolver::DefinitionResolver(asIScriptEngine *engine, std::string_view sourceCode, int line, int character)
        : nativeEng(engine), originalText(sourceCode), targetLine(line), targetCharacter(character) {}

    bool DefinitionResolver::IsStructureDeclarationKeyword(std::string_view txt) const noexcept { return structureKeywords.contains(txt); }
    bool DefinitionResolver::IsStatementKeyword(std::string_view txt) const noexcept { return statementKeywords.contains(txt); }
    bool DefinitionResolver::IsStorageModifierKeyword(std::string_view txt) const noexcept { return storageModifiers.contains(txt); }
    bool DefinitionResolver::IsPrimitiveType(std::string_view txt) const noexcept { return primitiveTypes.contains(txt); }
    bool DefinitionResolver::IsWord(std::string_view s) const noexcept { return !s.empty() && (std::isalnum(static_cast<unsigned char>(s[0])) || s[0] == '_'); }
    bool DefinitionResolver::IsCallableOrFieldSemantic(std::string_view typeLabel) const noexcept { return typeLabel == "function" || typeLabel == "property" || typeLabel == "lambda"; }
    bool DefinitionResolver::IsAutoDeducibleType(std::string_view typeStr) const noexcept { return typeStr.find("auto") != std::string_view::npos; }
    bool DefinitionResolver::IsValidTemplateToken(std::string_view tokenText) const noexcept { return tokenText == "::" || tokenText == "@" || tokenText == "&" || tokenText == "," || tokenText == "[" || tokenText == "]" || tokenText == "?" || IsWord(tokenText); }

    std::string_view DefinitionResolver::StripAccessModifiers(std::string_view typeStr) noexcept
    {
        if (typeStr.starts_with("protected "))
            return typeStr.substr(10);
        if (typeStr.starts_with("private "))
            return typeStr.substr(8);
        if (typeStr.starts_with("external "))
            return typeStr.substr(9);
        if (typeStr.starts_with("public "))
            return typeStr.substr(7);
        if (typeStr.starts_with("shared "))
            return typeStr.substr(7);

        return typeStr;
    }

    std::string_view DefinitionResolver::ExtractBaseTypeName(std::string_view typeStr) noexcept
    {
        size_t pos = typeStr.find_first_of("@& <");

        if (pos != std::string_view::npos)
            return typeStr.substr(0, pos);

        return typeStr;
    }

    bool DefinitionResolver::MatchesQual(const DeclInfo &decl, const std::string &fullQualName) const noexcept
    {
        if (decl.fullName == fullQualName)
            return true;

        if (decl.fullName.length() > fullQualName.length())
        {
            size_t p = decl.fullName.rfind(fullQualName);

            if (p != std::string::npos && p + fullQualName.length() == decl.fullName.length())
            {
                if (p >= 2 && decl.fullName.substr(p - 2, 2) == "::")
                    return true;
            }
        }

        return false;
    }

    void DefinitionResolver::TokenizePass()
    {
        int curLine = 0;
        int curChar = 0;
        size_t pos = 0;

        while (pos < originalText.length())
        {
            asUINT len = 0;
            asETokenClass tc = nativeEng->ParseToken(originalText.data() + pos, static_cast<asUINT>(originalText.length() - pos), &len);

            if (len == 0)
            {
                len = 1;
                tc = asTC_UNKNOWN;
            }

            std::string_view tokText = originalText.substr(pos, len);

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
    }

    int DefinitionResolver::FindTargetTokenIdx() noexcept
    {
        for (int k = 0; k < static_cast<int>(allTokens.size()); ++k)
        {
            int startLine = allTokens[k].line;
            int startChar = allTokens[k].character;
            std::string_view txt = allTokens[k].text;

            size_t segmentStart = 0;
            int currentLineInToken = startLine;

            for (size_t i = 0; i <= txt.length(); ++i)
            {
                if (i == txt.length() || txt[i] == '\n')
                {
                    int currentLineLength = static_cast<int>(i - segmentStart);
                    int lineStartChar = (currentLineInToken == startLine) ? startChar : 0;
                    int lineEndChar = lineStartChar + currentLineLength;

                    if (targetLine == currentLineInToken && targetCharacter >= lineStartChar && targetCharacter <= lineEndChar)
                    {
                        return k;
                    }

                    segmentStart = i + 1;
                    currentLineInToken++;
                }
            }
        }

        return -1;
    }

    bool DefinitionResolver::ParseType(size_t startIdx, size_t &nextIdx, std::string &typeStr)
    {
        size_t idx = startIdx;

        if (idx >= allTokens.size())
            return false;

        while (idx < allTokens.size() && IsStorageModifierKeyword(allTokens[idx].text))
        {
            idx++;
        }

        if (idx >= allTokens.size())
            return false;

        if (allTokens[idx].text == "const")
        {
            typeStr += "const ";
            idx++;
        }

        std::vector<size_t> typeTokenIndices;

        while (idx < allTokens.size())
        {
            std::string_view txt = allTokens[idx].text;

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

        if (typeTokenIndices.empty() || allTokens[typeTokenIndices.back()].text == "::")
            return false;

        int continuousWords = 0;

        for (int k = static_cast<int>(typeTokenIndices.size()) - 1; k >= 0; --k)
        {
            if (IsWord(allTokens[typeTokenIndices[k]].text) && allTokens[typeTokenIndices[k]].text != "::")
                continuousWords++;
            else
                break;
        }

        if (continuousWords >= 2)
        {
            size_t actualTypeEndCount = typeTokenIndices.size() - 1;
            idx = typeTokenIndices[actualTypeEndCount];
            typeTokenIndices.resize(actualTypeEndCount);
        }

        if (typeTokenIndices.empty())
            return false;

        for (size_t tIdx = 0; tIdx < typeTokenIndices.size(); ++tIdx)
        {
            typeStr += allTokens[typeTokenIndices[tIdx]].text;
        }

        if (idx < allTokens.size() && allTokens[idx].text == "<")
        {
            std::string tempTemplateStr = "<";
            size_t templateIdx = idx + 1;
            int depth = 1;
            bool isLegitTemplate = true;

            while (templateIdx < allTokens.size() && depth > 0)
            {
                std::string_view txt = allTokens[templateIdx].text;

                if (txt == "<")
                    depth++;
                else if (txt == ">")
                    depth--;
                else if (!IsValidTemplateToken(txt))
                {
                    isLegitTemplate = false;
                    break;
                }

                tempTemplateStr += txt;
                templateIdx++;
            }

            if (!isLegitTemplate || depth > 0)
                return false;

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
    }

    void DefinitionResolver::StructuralParsingPass()
    {
        std::vector<ScopeFrame> openFrames;
        tokenScopePrefixes.resize(allTokens.size(), "");

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
                int look = static_cast<int>(k) - 1;
                std::string fType = "other";
                std::string fName = "";
                bool hasParen = false;

                while (look >= 0)
                {
                    std::string_view t = allTokens[look].text;

                    if (t == "}" || t == ";")
                        break;
                    if (t == ")")
                        hasParen = true;

                    if (IsStructureDeclarationKeyword(t))
                    {
                        fType = std::string(t);

                        if (look + 1 < static_cast<int>(allTokens.size()))
                            fName = std::string(allTokens[look + 1].text);
                        if (t == "class" && look > 0 && allTokens[look - 1].text == "mixin")
                            fType = "mixin class";
                        if (t == "class" && look > 0 && allTokens[look - 1].text == "abstract")
                            fType = "abstract class";

                        break;
                    }
                    look--;
                }

                if (fType == "other" && hasParen)
                {
                    int sp = static_cast<int>(k) - 1;

                    while (sp >= 0 && allTokens[sp].text != "(")
                    {
                        sp--;
                    }

                    if (sp > 0 && IsWord(allTokens[sp - 1].text))
                    {
                        std::string_view possibleFuncName = allTokens[sp - 1].text;

                        if (!IsStatementKeyword(possibleFuncName) && possibleFuncName != "cast")
                        {
                            fType = "function";
                            fName = std::string(possibleFuncName);

                            if (sp >= 2 && allTokens[sp - 2].text == "~")
                                fName = "~" + fName;
                        }
                        else if (possibleFuncName == "function")
                        {
                            fType = "function";
                            fName = "lambda";
                        }
                    }
                }

                openFrames.push_back({fType, fName, k, allTokens[k].startPos});

                if (fType != "other" && fType != "function")
                {
                    if (fType == "namespace")
                    {
                        declarations.push_back({fName, currentPrefix + fName, "namespace", "namespace " + currentPrefix + fName, allTokens[k].startPos, std::string::npos, allTokens[k].startPos});
                    }
                    else if (fType == "enum")
                    {
                        ProcessEnumRule(k, fName, currentPrefix);
                    }
                    else
                    {
                        ProcessClassAndInterfaceRule(k, fName, fType, currentPrefix, static_cast<size_t>(look));
                    }
                }
            }
            else if (allTokens[k].text == "}")
            {
                if (!openFrames.empty())
                {
                    ScopeFrame top = openFrames.back();
                    openFrames.pop_back();

                    if (top.type != "other")
                    {
                        structuralScopes.push_back({top.type, top.name, tokenScopePrefixes[top.openBraceIdx] + top.name, top.startPos, allTokens[k].endPos});

                        for (auto &decl : declarations)
                        {
                            if (decl.name == top.name && decl.type == top.type && decl.scopeStart == top.startPos)
                            {
                                decl.scopeEnd = allTokens[k].endPos;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    void DefinitionResolver::ProcessEnumRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix)
    {
        declarations.push_back({fName, currentPrefix + fName, "enum", "enum " + currentPrefix + fName, allTokens[openBraceIdx].startPos, std::string::npos, allTokens[openBraceIdx].startPos});
        size_t enumScan = openBraceIdx + 1;
        int enumVal = 0;

        while (enumScan < allTokens.size() && allTokens[enumScan].text != "}")
        {
            if (allTokens[enumScan].tokenClass == asTC_IDENTIFIER && IsWord(allTokens[enumScan].text))
            {
                std::string eNameStr = std::string(allTokens[enumScan].text);
                declarations.push_back({eNameStr, currentPrefix + fName + "::" + eNameStr, "enum_value", "enum " + currentPrefix + fName + "::" + eNameStr + " = " + std::to_string(enumVal), allTokens[enumScan].startPos, allTokens[enumScan].endPos, allTokens[enumScan].startPos});
                enumVal++;
            }
            enumScan++;
        }
    }

    void DefinitionResolver::ProcessClassAndInterfaceRule(size_t openBraceIdx, const std::string &fName, const std::string &fType, const std::string &currentPrefix, size_t lookaheadIdx)
    {
        declarations.push_back({fName, currentPrefix + fName, fType, fType + " " + currentPrefix + fName, allTokens[openBraceIdx].startPos, std::string::npos, allTokens[openBraceIdx].startPos});
        size_t inheritScan = lookaheadIdx + 2;

        while (inheritScan < openBraceIdx)
        {
            std::string_view tokenText = allTokens[inheritScan].text;

            if (IsWord(tokenText) && !IsStorageModifierKeyword(tokenText))
            {
                classInheritanceMapper[fName].push_back(std::string(tokenText));
            }
            inheritScan++;
        }
    }

    void DefinitionResolver::ProcessFuncDefRule(size_t &idxScan, const std::string &currentPrefix)
    {
        size_t nextIdx = idxScan + 1;
        std::string typeStr = "";

        if (ParseType(idxScan + 1, nextIdx, typeStr))
        {
            if (nextIdx < allTokens.size() && allTokens[nextIdx].text == "@")
                nextIdx++;

            if (nextIdx < allTokens.size() && IsWord(allTokens[nextIdx].text))
            {
                std::string fdName = std::string(allTokens[nextIdx].text);
                std::string fullDecl = "funcdef " + typeStr + " " + fdName;
                size_t paramScan = nextIdx + 1;

                while (paramScan < allTokens.size() && allTokens[paramScan].text != ";")
                {
                    fullDecl += " ";
                    fullDecl += allTokens[paramScan].text;
                    paramScan++;
                }

                size_t endP = (paramScan < allTokens.size()) ? allTokens[paramScan].endPos : allTokens[paramScan - 1].endPos;
                declarations.push_back({fdName, currentPrefix + fdName, "funcdef", fullDecl, allTokens[nextIdx].startPos, endP, allTokens[nextIdx].startPos});
                idxScan = (paramScan < allTokens.size()) ? paramScan + 1 : paramScan;
                return;
            }
        }

        idxScan++;
    }

    void DefinitionResolver::ProcessForLoopRule(size_t &idxScan)
    {
        size_t loopIdx = idxScan + 1;

        if (loopIdx < allTokens.size() && allTokens[loopIdx].text == "(")
        {
            size_t nextIdx = loopIdx + 1;
            std::string loopTypeStr = "";

            if (ParseType(loopIdx + 1, nextIdx, loopTypeStr))
            {
                if (nextIdx < allTokens.size() && IsWord(allTokens[nextIdx].text) && !reservedKeywords.contains(allTokens[nextIdx].text))
                {
                    std::string loopVarName = std::string(allTokens[nextIdx].text);
                    size_t nameTokenIdx = nextIdx;
                    size_t activeFuncStart = std::string::npos;
                    size_t activeFuncEnd = std::string::npos;
                    size_t entPos = allTokens[nameTokenIdx].startPos;

                    for (const auto &scope : structuralScopes)
                    {
                        if (entPos >= scope.startPos && entPos <= scope.endPos && scope.type == "function")
                        {
                            if (activeFuncStart == std::string::npos || scope.startPos > activeFuncStart)
                            {
                                activeFuncStart = scope.startPos;
                                activeFuncEnd = scope.endPos;
                            }
                        }
                    }

                    if (activeFuncStart != std::string::npos)
                    {
                        declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[nameTokenIdx].startPos, activeFuncEnd, allTokens[nameTokenIdx].startPos});
                    }
                }
            }
        }

        idxScan++;
    }

    void DefinitionResolver::ProcessForeachLoopRule(size_t &idxScan)
    {
        size_t loopIdx = idxScan + 1;

        if (loopIdx < allTokens.size() && allTokens[loopIdx].text == "(")
        {
            size_t nextIdx = loopIdx + 1;
            std::string loopTypeStr = "";

            if (ParseType(loopIdx + 1, nextIdx, loopTypeStr))
            {
                if (nextIdx < allTokens.size() && IsWord(allTokens[nextIdx].text) && !reservedKeywords.contains(allTokens[nextIdx].text))
                {
                    std::string loopVarName = std::string(allTokens[nextIdx].text);
                    size_t nameTokenIdx = nextIdx;
                    size_t activeFuncStart = std::string::npos;
                    size_t activeFuncEnd = std::string::npos;
                    size_t entPos = allTokens[nameTokenIdx].startPos;

                    for (const auto &scope : structuralScopes)
                    {
                        if (entPos >= scope.startPos && entPos <= scope.endPos && scope.type == "function")
                        {
                            if (activeFuncStart == std::string::npos || scope.startPos > activeFuncStart)
                            {
                                activeFuncStart = scope.startPos;
                                activeFuncEnd = scope.endPos;
                            }
                        }
                    }

                    if (activeFuncStart != std::string::npos)
                    {
                        declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[nameTokenIdx].startPos, activeFuncEnd, allTokens[nameTokenIdx].startPos});
                    }
                }
            }
        }

        idxScan++;
    }

    void DefinitionResolver::ProcessScriptRule()
    {
        size_t idxScan = 0;

        while (idxScan < allTokens.size())
        {
            std::string_view txt = allTokens[idxScan].text;
            std::string currentPrefix = tokenScopePrefixes[idxScan];

            if (IsStructureDeclarationKeyword(txt) || IsStorageModifierKeyword(txt))
            {
                idxScan++;
                continue;
            }

            if (txt == "typedef" && idxScan + 2 < allTokens.size())
            {
                declarations.push_back({std::string(allTokens[idxScan + 2].text), std::string(allTokens[idxScan + 2].text), "typedef", "typedef " + std::string(allTokens[idxScan + 1].text) + " " + std::string(allTokens[idxScan + 2].text), allTokens[idxScan + 2].startPos, allTokens[idxScan + 2].endPos, allTokens[idxScan + 2].startPos});
                idxScan += 3;
                continue;
            }

            if (txt == "foreach" || txt == "for")
            {
                if (txt == "for")
                    ProcessForLoopRule(idxScan);
                else
                    ProcessForeachLoopRule(idxScan);

                continue;
            }

            if (txt == "funcdef")
            {
                ProcessFuncDefRule(idxScan, currentPrefix);
                continue;
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

                std::string_view baseType = allTokens[idxScan].text;

                if (IsStatementKeyword(baseType))
                {
                    idxScan++;
                    continue;
                }

                bool statementMatched = false;
                size_t innerScan = nextIdx;

                while (innerScan < allTokens.size() && IsWord(allTokens[innerScan].text) && !reservedKeywords.contains(allTokens[innerScan].text))
                {
                    size_t nameIdx = innerScan;
                    std::string entityName = std::string(allTokens[nameIdx].text);
                    size_t activeFuncStart = std::string::npos;
                    size_t activeFuncEnd = std::string::npos;
                    size_t activeClassStart = std::string::npos;
                    size_t activeClassEnd = std::string::npos;
                    size_t entPos = allTokens[nameIdx].startPos;

                    for (const auto &scope : structuralScopes)
                    {
                        if (entPos >= scope.startPos && entPos <= scope.endPos)
                        {
                            if (scope.type == "function" && (activeFuncStart == std::string::npos || scope.startPos > activeFuncStart))
                            {
                                activeFuncStart = scope.startPos;
                                activeFuncEnd = scope.endPos;
                            }
                            else if (scope.type != "function" && (activeClassStart == std::string::npos || scope.startPos > activeClassStart))
                            {
                                activeClassStart = scope.startPos;
                                activeClassEnd = scope.endPos;
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
                            if (contextualKeywords.contains(allTokens[modScan].text) || reservedKeywords.contains(allTokens[modScan].text))
                            {
                                modifiers += " ";
                                modifiers += allTokens[modScan].text;
                            }
                            modScan++;
                        }

                        bool isFunctionDef = (modScan < allTokens.size() && allTokens[modScan].text == "{") || (activeClassStart != std::string::npos && activeFuncStart == std::string::npos) || (activeClassStart == std::string::npos && activeFuncStart == std::string::npos && modifiers.find("import") != std::string::npos);

                        if (isFunctionDef)
                        {
                            std::vector<FuncParam> funcParams;
                            std::vector<TokenInfo> pToks;

                            for (size_t kLoop = nameIdx + 2; kLoop < closeParen; ++kLoop)
                            {
                                if (allTokens[kLoop].text == "," || kLoop == closeParen - 1)
                                {
                                    if (kLoop == closeParen - 1 && allTokens[kLoop].text != ",")
                                        pToks.push_back(allTokens[kLoop]);

                                    if (!pToks.empty() && IsWord(pToks.back().text) && !reservedKeywords.contains(pToks.back().text))
                                    {
                                        TokenInfo nTok = pToks.back();
                                        pToks.pop_back();
                                        std::string ptStr = "";

                                        for (size_t tIndex = 0; tIndex < pToks.size(); ++tIndex)
                                        {
                                            if (tIndex > 0 && pToks[tIndex].text != "@" && pToks[tIndex].text != "&" && pToks[tIndex - 1].text != "::")
                                                ptStr += " ";

                                            ptStr += pToks[tIndex].text;
                                        }
                                        funcParams.push_back({std::string(nTok.text), ptStr});
                                    }

                                    pToks.clear();
                                }
                                else
                                {
                                    pToks.push_back(allTokens[kLoop]);
                                }
                            }

                            size_t functionEndPos = allTokens[closeParen].endPos;

                            if (modScan < allTokens.size() && allTokens[modScan].text == "{")
                            {
                                for (const auto &funcScope : structuralScopes)
                                {
                                    if (funcScope.type == "function" && funcScope.name == entityName && funcScope.startPos == allTokens[modScan].startPos)
                                    {
                                        functionEndPos = funcScope.endPos;
                                        break;
                                    }
                                }
                            }

                            std::string paramsStr = "";

                            for (size_t pIndex = 0; pIndex < funcParams.size(); ++pIndex)
                            {
                                if (pIndex > 0)
                                    paramsStr += ", ";

                                paramsStr += funcParams[pIndex].pType + " " + funcParams[pIndex].pName;
                            }

                            declarations.push_back({entityName, currentPrefix + entityName, "function", typeStr + " " + currentPrefix + entityName + "(" + paramsStr + ")" + modifiers, allTokens[nameIdx].startPos, functionEndPos, allTokens[nameIdx].startPos});

                            if (functionEndPos != std::string::npos)
                            {
                                for (const auto &fp : funcParams)
                                    declarations.push_back({fp.pName, fp.pName, "parameter", fp.pType + " " + fp.pName, allTokens[nameIdx].startPos, functionEndPos, allTokens[nameIdx].startPos});
                            }

                            idxScan = (modScan < allTokens.size() && allTokens[modScan].text == "{") ? modScan + 1 : modScan;
                            statementMatched = true;
                            break;
                        }
                        else
                        {
                            std::string finalTypeStr = typeStr;

                            if (activeFuncStart != std::string::npos)
                                declarations.push_back({entityName, entityName, "local_variable", finalTypeStr + " " + entityName, activeFuncStart, activeFuncEnd, allTokens[nameIdx].startPos});
                            else if (activeClassStart != std::string::npos)
                                declarations.push_back({entityName, currentPrefix + entityName, "property", finalTypeStr + " " + currentPrefix + entityName, activeClassStart, activeClassEnd, allTokens[nameIdx].startPos});
                            else
                                declarations.push_back({entityName, currentPrefix + entityName, "global_variable", finalTypeStr + " " + currentPrefix + entityName, allTokens[nameIdx].startPos, std::string::npos, allTokens[nameIdx].startPos});

                            size_t searchComma = nameIdx + 1;

                            while (searchComma < allTokens.size() && allTokens[searchComma].text != "," && allTokens[searchComma].text != ";")
                            {
                                searchComma++;
                            }

                            if (searchComma < allTokens.size() && allTokens[searchComma].text == ",")
                            {
                                idxScan = searchComma + 1;
                                statementMatched = true;
                                break;
                            }

                            idxScan = (searchComma < allTokens.size()) ? searchComma + 1 : searchComma;
                            statementMatched = true;
                            break;
                        }
                    }
                    else if (nameIdx + 1 < allTokens.size() && (allTokens[nameIdx + 1].text == ";" || allTokens[nameIdx + 1].text == "=" || allTokens[nameIdx + 1].text == "," || allTokens[nameIdx + 1].text == "[" || allTokens[nameIdx + 1].text == ")"))
                    {
                        std::string finalTypeStr = typeStr;

                        if (activeFuncStart != std::string::npos)
                            declarations.push_back({entityName, entityName, "local_variable", finalTypeStr + " " + entityName, activeFuncStart, activeFuncEnd, allTokens[nameIdx].startPos});
                        else if (activeClassStart != std::string::npos)
                            declarations.push_back({entityName, currentPrefix + entityName, "property", finalTypeStr + " " + currentPrefix + entityName, activeClassStart, activeClassEnd, allTokens[nameIdx].startPos});
                        else
                            declarations.push_back({entityName, currentPrefix + entityName, "global_variable", finalTypeStr + " " + currentPrefix + entityName, allTokens[nameIdx].startPos, std::string::npos, allTokens[nameIdx].startPos});

                        size_t searchComma = nameIdx + 1;

                        while (searchComma < allTokens.size() && allTokens[searchComma].text != "," && allTokens[searchComma].text != ";")
                        {
                            searchComma++;
                        }

                        if (searchComma < allTokens.size() && allTokens[searchComma].text == ",")
                        {
                            idxScan = searchComma + 1;
                            statementMatched = true;
                            break;
                        }

                        idxScan = (searchComma < allTokens.size()) ? searchComma + 1 : searchComma;
                        statementMatched = true;
                        break;
                    }
                    break;
                }

                if (statementMatched)
                    continue;

                idxScan = nextIdx;
                continue;
            }

            idxScan++;
        }
    }

    void DefinitionResolver::ProcessLambdaRule(size_t &k)
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

            std::vector<TokenInfo> pToks;
            std::vector<FuncParam> funcParams;

            for (size_t idxP = nameIdx + 2; idxP < closeParen; ++idxP)
            {
                if (allTokens[idxP].text == "," || idxP == closeParen - 1)
                {
                    if (idxP == closeParen - 1 && allTokens[idxP].text != ",")
                        pToks.push_back(allTokens[idxP]);

                    if (!pToks.empty() && IsWord(pToks.back().text) && !reservedKeywords.contains(pToks.back().text))
                    {
                        TokenInfo nTok = pToks.back();
                        pToks.pop_back();
                        std::string ptStr = "";

                        for (size_t t = 0; t < pToks.size(); ++t)
                        {
                            if (t > 0 && pToks[t].text != "@" && pToks[t].text != "&" && pToks[t - 1].text != "::")
                                ptStr += " ";

                            ptStr += pToks[t].text;
                        }
                        funcParams.push_back({std::string(nTok.text), ptStr});
                    }
                    pToks.clear();
                }
                else
                {
                    pToks.push_back(allTokens[idxP]);
                }
            }

            size_t functionEndPos = allTokens[closeParen].endPos;

            if (closeParen + 1 < allTokens.size() && allTokens[closeParen + 1].text == "{")
            {
                for (const auto &scope : structuralScopes)
                {
                    if (scope.type == "function" && scope.startPos == allTokens[closeParen + 1].startPos)
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
                    paramsStr += ", ";

                paramsStr += funcParams[p].pType + " " + funcParams[p].pName;
            }

            declarations.push_back({"function", "function", "lambda", "auto function(" + paramsStr + ")", allTokens[nameIdx].startPos, functionEndPos, allTokens[nameIdx].startPos});

            if (functionEndPos != std::string::npos)
            {
                for (const auto &fp : funcParams)
                    declarations.push_back({fp.pName, fp.pName, "parameter", fp.pType + " " + fp.pName, allTokens[nameIdx].startPos, functionEndPos, allTokens[nameIdx].startPos});
            }

            k = closeParen;
        }
    }

    size_t DefinitionResolver::ResolveDefinitionPosition(int targetIdx, size_t &outLength)
    {
        outLength = 0;

        if (targetIdx == -1)
            return std::string_view::npos;

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
                objName = std::string(allTokens[backIdx].text);

            std::string objType = "";

            for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                if (it->name == objName && (it->type == "local_variable" || it->type == "parameter" || it->type == "property" || it->type == "global_variable"))
                {
                    if ((it->type != "local_variable" && it->type != "parameter") || (targetTok.startPos >= it->scopeStart && targetTok.startPos <= it->scopeEnd))
                    {
                        size_t lastSpace = it->hoverText.rfind(' ');

                        if (lastSpace != std::string::npos)
                        {
                            std::string_view accessStrippedView = StripAccessModifiers(std::string_view(it->hoverText).substr(0, lastSpace));
                            objType = std::string(accessStrippedView);
                        }
                        break;
                    }
                }
            }

            if (!objType.empty())
            {
                std::string_view cleanTypeName = ExtractBaseTypeName(objType);
                std::vector<std::string> searchHierarchy = {std::string(cleanTypeName)};

                if (classInheritanceMapper.contains(std::string(cleanTypeName)))
                {
                    for (const auto &baseClass : classInheritanceMapper[std::string(cleanTypeName)])
                        searchHierarchy.push_back(baseClass);
                }

                for (const auto &typeHierarchyStr : searchHierarchy)
                {
                    for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                    {
                        if (it->name == targetTok.text && IsCallableOrFieldSemantic(it->type) && it->fullName.find(typeHierarchyStr + "::") != std::string::npos)
                        {
                            outLength = targetTok.text.length();
                            return it->defPos;
                        }
                    }
                }
            }

            return std::string_view::npos;
        }

        if ((reservedKeywords.contains(targetTok.text) || contextualKeywords.contains(targetTok.text)) && !primitiveTypes.contains(targetTok.text))
        {
            if (targetTok.text != "this" && targetTok.text != "super" && targetTok.text != "function")
                return std::string_view::npos;
        }

        if (targetTok.tokenClass == asTC_IDENTIFIER)
        {
            std::string fullQualName = std::string(targetTok.text);
            int left = targetIdx - 1;

            while (left >= 0 && allTokens[left].text == "::")
            {
                if (left > 0 && IsWord(allTokens[left - 1].text))
                {
                    fullQualName = std::string(allTokens[left - 1].text) + "::" + fullQualName;
                    left -= 2;
                }
                else
                {
                    break;
                }
            }

            bool isFollowedByParen = (targetIdx + 1 < static_cast<int>(allTokens.size()) && allTokens[targetIdx + 1].text == "(");
            bool isPrecededByTilde = (targetIdx > 0 && allTokens[targetIdx - 1].text == "~");

            for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                if ((it->type == "local_variable" || it->type == "parameter" || it->type == "global_variable") && it->name == targetTok.text)
                {
                    if (it->type == "global_variable" || (targetTok.startPos >= it->scopeStart && targetTok.startPos <= it->scopeEnd))
                    {
                        outLength = targetTok.text.length();
                        return it->defPos;
                    }
                }
            }

            if (isFollowedByParen || isPrecededByTilde || targetTok.text == "function")
            {
                for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                {
                    if ((it->type == "function" || it->type == "lambda") && MatchesQual(*it, fullQualName))
                    {
                        if (it->type == "lambda" && targetTok.startPos != it->defPos)
                            continue;

                        outLength = targetTok.text.length();
                        return it->defPos;
                    }
                }
            }

            for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                if (it->type != "function" && it->type != "local_variable" && it->type != "parameter" && it->type != "global_variable" && it->type != "lambda" && MatchesQual(*it, fullQualName))
                {
                    outLength = targetTok.text.length();
                    return it->defPos;
                }
            }
        }

        return std::string_view::npos;
    }

    json CreateLocationJson(std::string_view uri, std::string_view sourceCode, size_t offset, size_t wordLength)
    {
        int line = 0;
        int character = 0;
        size_t lastNewline = 0;

        for (size_t i = 0; i < offset; ++i)
        {
            if (sourceCode[i] == '\n')
            {
                line++;
                lastNewline = i + 1;
            }
        }
        character = static_cast<int>(offset - lastNewline);

        json start = json::object();
        start["line"] = line;
        start["character"] = character;

        json end = json::object();
        end["line"] = line;
        end["character"] = character + static_cast<int>(wordLength);

        json range = json::object();
        range["start"] = start;
        range["end"] = end;

        json location = json::object();
        location["uri"] = std::string(uri);
        location["range"] = range;

        return location;
    }
}

json DefinitionHandler::HandleDefinitionRequest(asIScriptEngine *engine, const json &request, std::string_view sourceCode)
{
    if (!request.contains(KEY_PARAMS))
        return nullptr;

    const auto &params = request[std::string(KEY_PARAMS)];

    if (!params.contains(KEY_TEXT_DOCUMENT) || !params.contains(KEY_POSITION))
        return nullptr;

    const auto &textDoc = params[std::string(KEY_TEXT_DOCUMENT)];
    const auto &pos = params[std::string(KEY_POSITION)];

    if (!textDoc.contains(KEY_URI) || !pos.contains(KEY_LINE) || !pos.contains(KEY_CHARACTER))
        return nullptr;

    std::string_view uri = textDoc[std::string(KEY_URI)].get<std::string_view>();
    int line = pos[std::string(KEY_LINE)].get<int>();
    int character = pos[std::string(KEY_CHARACTER)].get<int>();

    DefinitionUtils::DefinitionResolver resolver(engine, sourceCode, line, character);

    resolver.TokenizePass();
    int targetIdx = resolver.FindTargetTokenIdx();

    resolver.StructuralParsingPass();
    resolver.ProcessScriptRule();

    for (size_t k = 0; k < resolver.allTokens.size(); ++k)
    {
        if (resolver.allTokens[k].text == "function")
        {
            resolver.ProcessLambdaRule(k);
        }
    }

    size_t matchLength = 0;
    size_t defOffset = resolver.ResolveDefinitionPosition(targetIdx, matchLength);

    if (defOffset != std::string_view::npos && matchLength > 0)
    {
        return DefinitionUtils::CreateLocationJson(uri, sourceCode, defOffset, matchLength);
    }

    return nullptr;
}