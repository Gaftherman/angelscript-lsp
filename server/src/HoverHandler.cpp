/**
 * @file HoverHandler.cpp
 * @brief Implementation of the HoverHandler class for language server hover functionality.
 */

#include "HoverHandler.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fmt/core.h>

const std::unordered_set<std::string> HoverHandler::reservedKeywords = {
    "and", "auto", "bool", "break", "case", "cast", "catch", "class", "const", "continue",
    "default", "do", "double", "else", "enum", "false", "float", "for", "foreach", "funcdef",
    "if", "import", "in", "inout", "int", "interface", "int8", "int16", "int32", "int64",
    "is", "mixin", "namespace", "not", "null", "or", "out", "private", "protected", "return",
    "switch", "true", "try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64", "using",
    "void", "while", "xor"};

const std::unordered_set<std::string> HoverHandler::contextualKeywords = {
    "abstract", "delete", "explicit", "external", "final", "from", "function", "get",
    "override", "property", "set", "shared", "super", "this"};

const std::unordered_set<std::string> HoverHandler::primitiveTypes = {
    "void", "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64", "float", "double", "bool", "auto"};

const std::unordered_set<std::string> HoverHandler::storageModifiers = {
    "private", "protected", "public", "shared", "external"};

HoverHandler::HoverHandler(asIScriptEngine *engine, const std::string &sourceCode, int line, int character)
    : nativeEng(engine), originalText(sourceCode), targetLine(line), targetCharacter(character)
{
}

bool HoverHandler::IsWord(const std::string_view &s) const
{
    char c;
    c = '\0';
    if (s.empty())
        return false;
    c = s[0];
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string_view HoverHandler::StripAccessModifiers(std::string_view typeStr) noexcept
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

std::string HoverHandler::ExtractBaseTypeName(std::string_view typeStr)
{
    std::string baseName;
    size_t i;
    char c;

    baseName = "";
    i = 0;
    for (i = 0; i < typeStr.length(); ++i)
    {
        c = typeStr[i];
        if (c == '@' || c == '&' || c == ' ' || c == '<')
        {
            break;
        }
        baseName += c;
    }
    return baseName;
}

bool HoverHandler::IsKeywordHoverException(std::string_view tokenText) const noexcept
{
    return tokenText == "this" || tokenText == "super" || tokenText == "function";
}

bool HoverHandler::IsCallableOrFieldSemantic(std::string_view typeLabel) const noexcept
{
    return typeLabel == "function" || typeLabel == "property" || typeLabel == "lambda";
}

bool HoverHandler::IsAutoDeducibleType(std::string_view typeStr) const noexcept
{
    return typeStr.find("auto") != std::string_view::npos;
}

bool HoverHandler::IsValidTemplateToken(std::string_view tokenText) const noexcept
{
    return tokenText == "::" || tokenText == "@" || tokenText == "&" ||
           tokenText == "," || tokenText == "[" || tokenText == "]" ||
           tokenText == "?" || IsWord(std::string(tokenText));
}

void HoverHandler::NormalizeSignatureSpacing(std::string &signature) const
{
    size_t p;
    p = 0;

    while ((p = signature.find(" ::")) != std::string::npos)
        signature.erase(p, 1);
    while ((p = signature.find(":: ")) != std::string::npos)
        signature.erase(p + 2, 1);

    while ((p = signature.find("& in")) != std::string::npos)
        signature.replace(p, 4, "&in");
    while ((p = signature.find("& out")) != std::string::npos)
        signature.replace(p, 5, "&out");
    while ((p = signature.find("& inout")) != std::string::npos)
        signature.replace(p, 7, "&inout");
}

std::string HoverHandler::CleanSignature(std::string str)
{
    std::string res;
    std::string finalRes;
    size_t idx;
    char c;

    res = "";
    finalRes = "";

    for (idx = 0; idx < str.size(); ++idx)
    {
        c = str[idx];
        if (c == ' ')
        {
            if (!res.empty() && (res.back() == '<' || res.back() == '>' || res.back() == '@' || res.back() == '&' || res.back() == '('))
                continue;
            if (idx + 1 < str.size() && (str[idx + 1] == '<' || str[idx + 1] == '>' || str[idx + 1] == '@' || str[idx + 1] == '&' || str[idx + 1] == '(' || str[idx + 1] == ',' || str[idx + 1] == ')'))
                continue;
        }
        res += c;
    }

    for (idx = 0; idx < res.size(); ++idx)
    {
        finalRes += res[idx];
        if (res[idx] == '@' || res[idx] == '&' || res[idx] == ',')
        {
            if (idx + 1 < res.size() && res[idx + 1] != ' ' && res[idx + 1] != ',' && res[idx + 1] != ')' && res[idx + 1] != '>')
                finalRes += ' ';
        }
        if (res[idx] == '>')
        {
            if (idx + 1 < res.size() && res[idx + 1] != ' ' && res[idx + 1] != ',' && res[idx + 1] != ')' && res[idx + 1] != '>' && res[idx + 1] != '@' && res[idx + 1] != '&' && res[idx + 1] != ':')
                finalRes += ' ';
        }
    }

    NormalizeSignatureSpacing(finalRes);
    return finalRes;
}

bool HoverHandler::ParseType(size_t startIdx, size_t &nextIdx, std::string &typeStr)
{
    size_t idx;
    int continuousWords;
    std::string txt;
    int k;
    size_t actualTypeEndCount;
    size_t tIdx;
    std::string tempTemplateStr;
    size_t templateIdx;
    int depth;
    bool isLegitTemplate;
    std::vector<size_t> typeTokenIndices;

    idx = startIdx;
    continuousWords = 0;

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

    while (idx < allTokens.size())
    {
        txt = allTokens[idx].text;
        if (IsWord(txt) || txt == "::" || txt == "?")
        {
            typeTokenIndices.push_back(idx);
            idx++;
        }
        else
            break;
    }

    if (typeTokenIndices.empty())
        return false;
    if (allTokens[typeTokenIndices.back()].text == "::")
        return false;

    for (k = static_cast<int>(typeTokenIndices.size()) - 1; k >= 0; --k)
    {
        if (IsWord(allTokens[typeTokenIndices[k]].text) && allTokens[typeTokenIndices[k]].text != "::")
            continuousWords++;
        else
            break;
    }

    if (continuousWords >= 2)
    {
        actualTypeEndCount = typeTokenIndices.size() - 1;
        idx = typeTokenIndices[actualTypeEndCount];
        typeTokenIndices.resize(actualTypeEndCount);
    }

    if (typeTokenIndices.empty())
        return false;

    for (tIdx = 0; tIdx < typeTokenIndices.size(); ++tIdx)
    {
        typeStr += allTokens[typeTokenIndices[tIdx]].text;
    }

    if (idx < allTokens.size() && allTokens[idx].text == "<")
    {
        tempTemplateStr = "<";
        templateIdx = idx + 1;
        depth = 1;
        isLegitTemplate = true;

        while (templateIdx < allTokens.size() && depth > 0)
        {
            txt = allTokens[templateIdx].text;
            if (txt == "<")
                depth++;
            else if (txt == ">")
                depth--;
            else if (IsValidTemplateToken(txt))
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
                break;
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
            break;
    }

    nextIdx = idx;
    return true;
}

void HoverHandler::TokenizePass()
{
    int curLine;
    int curChar;
    size_t pos;
    asUINT len;
    asETokenClass tc;
    std::string tokText;
    size_t i;
    char c;

    curLine = 0;
    curChar = 0;
    pos = 0;

    while (pos < originalText.length())
    {
        len = 0;
        tc = nativeEng->ParseToken(originalText.data() + pos, static_cast<asUINT>(originalText.length() - pos), &len);

        if (len == 0)
        {
            len = 1;
            tc = asTC_UNKNOWN;
        }

        tokText = originalText.substr(pos, len);

        if (tc == asTC_WHITESPACE || tc == asTC_COMMENT)
        {
            for (i = 0; i < tokText.length(); ++i)
            {
                c = tokText[i];
                if (c == '\n')
                {
                    curLine++;
                    curChar = 0;
                }
                else
                    curChar++;
            }
            pos += len;
            continue;
        }

        allTokens.push_back({tokText, tc, pos, pos + len, curLine, curChar});

        for (i = 0; i < tokText.length(); ++i)
        {
            c = tokText[i];
            if (c == '\n')
            {
                curLine++;
                curChar = 0;
            }
            else
                curChar++;
        }
        pos += len;
    }
}

int HoverHandler::FindTargetTokenIdx()
{
    int k;
    int startLine;
    int startChar;
    std::string_view txt;
    size_t segmentStart;
    int currentLineInToken;
    size_t i;
    int currentLineLength;
    int lineStartChar;
    int lineEndChar;

    for (k = 0; k < static_cast<int>(allTokens.size()); ++k)
    {
        startLine = allTokens[k].line;
        startChar = allTokens[k].character;
        txt = allTokens[k].text;
        segmentStart = 0;
        currentLineInToken = startLine;

        for (i = 0; i <= txt.length(); ++i)
        {
            if (i == txt.length() || txt[i] == '\n')
            {
                currentLineLength = static_cast<int>(i - segmentStart);
                lineStartChar = (currentLineInToken == startLine) ? startChar : 0;
                lineEndChar = lineStartChar + currentLineLength;

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

void HoverHandler::StructuralParsingPass()
{
    std::vector<ScopeFrame> openFrames;
    size_t k;
    std::string currentPrefix;
    std::vector<ScopeFrame>::const_iterator frameIt;
    int look;
    std::string fType;
    std::string fName;
    bool hasParen;
    std::string t;
    int sp;
    std::string possibleFuncName;
    ScopeFrame top;
    std::vector<DeclInfo>::iterator declIt;

    tokenScopePrefixes.resize(allTokens.size(), "");

    for (k = 0; k < allTokens.size(); ++k)
    {
        currentPrefix = "";
        for (frameIt = openFrames.begin(); frameIt != openFrames.end(); ++frameIt)
        {
            if (frameIt->type == "namespace" || frameIt->type == "class" || frameIt->type == "interface" || frameIt->type == "mixin class" || frameIt->type == "abstract class")
            {
                currentPrefix += frameIt->name + "::";
            }
        }

        tokenScopePrefixes[k] = currentPrefix;

        if (allTokens[k].text == "{")
        {
            look = static_cast<int>(k) - 1;
            fType = "other";
            fName = "";
            hasParen = false;

            while (look >= 0)
            {
                t = allTokens[look].text;
                if (t == "}" || t == ";")
                    break;
                if (t == ")")
                    hasParen = true;

                if (IsStructureDeclarationKeyword(t))
                {
                    fType = t;
                    if (look + 1 < static_cast<int>(allTokens.size()))
                        fName = allTokens[look + 1].text;
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
                sp = static_cast<int>(k) - 1;
                while (sp >= 0 && allTokens[sp].text != "(")
                {
                    sp--;
                }

                if (sp > 0 && IsWord(allTokens[sp - 1].text))
                {
                    possibleFuncName = allTokens[sp - 1].text;

                    if (!IsStatementKeyword(possibleFuncName) && possibleFuncName != "cast")
                    {
                        fType = "function";
                        fName = possibleFuncName;

                        if (sp >= 2 && allTokens[sp - 2].text == "~")
                        {
                            fName = "~" + fName;
                        }
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
                    ProcessNamespaceRule(k, fName, currentPrefix);
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
                top = openFrames.back();
                openFrames.pop_back();

                if (top.type != "other")
                {
                    structuralScopes.push_back({top.type, top.name, tokenScopePrefixes[top.openBraceIdx] + top.name, top.startPos, allTokens[k].endPos});
                    for (declIt = declarations.begin(); declIt != declarations.end(); ++declIt)
                    {
                        if (declIt->name == top.name && declIt->type == top.type && declIt->startPos == top.startPos)
                        {
                            declIt->endPos = allTokens[k].endPos;
                            break;
                        }
                    }
                }
            }
        }
    }
}

void HoverHandler::ProcessNamespaceRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix)
{
    declarations.push_back({fName, currentPrefix + fName, "namespace", "namespace " + currentPrefix + fName, allTokens[openBraceIdx].startPos, std::string::npos});
}

void HoverHandler::ProcessEnumRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix)
{
    size_t enumScan;
    int enumVal;

    declarations.push_back({fName, currentPrefix + fName, "enum", "enum " + currentPrefix + fName, allTokens[openBraceIdx].startPos, std::string::npos});

    enumScan = openBraceIdx + 1;
    enumVal = 0;
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

void HoverHandler::ProcessClassAndInterfaceRule(size_t openBraceIdx, const std::string &fName, const std::string &fType, const std::string &currentPrefix, size_t lookaheadIdx)
{
    size_t inheritScan;
    std::string tokenText;

    declarations.push_back({fName, currentPrefix + fName, fType, fType + " " + currentPrefix + fName, allTokens[openBraceIdx].startPos, std::string::npos});

    if (fType == "class" || fType == "interface" || fType == "mixin class" || fType == "abstract class")
    {
        inheritScan = lookaheadIdx + 2;
        while (inheritScan < openBraceIdx)
        {
            tokenText = allTokens[inheritScan].text;
            if (IsWord(tokenText) && !IsStorageModifierKeyword(tokenText))
            {
                classInheritanceMapper[fName].push_back(tokenText);
            }
            inheritScan++;
        }
    }
}

void HoverHandler::ProcessFuncDefRule(size_t &idxScan, const std::string &currentPrefix)
{
    size_t nextIdx;
    std::string typeStr;
    std::string fdName;
    std::string fullDecl;
    size_t paramScan;
    size_t endP;

    nextIdx = idxScan + 1;
    typeStr = "";
    if (ParseType(idxScan + 1, nextIdx, typeStr))
    {
        if (nextIdx < allTokens.size() && allTokens[nextIdx].text == "@")
            nextIdx++;
        if (nextIdx < allTokens.size() && IsWord(allTokens[nextIdx].text))
        {
            fdName = allTokens[nextIdx].text;
            fullDecl = "funcdef " + typeStr + " " + fdName;
            paramScan = nextIdx + 1;
            while (paramScan < allTokens.size() && allTokens[paramScan].text != ";")
            {
                fullDecl += " " + allTokens[paramScan].text;
                paramScan++;
            }
            endP = (paramScan < allTokens.size()) ? allTokens[paramScan].endPos : allTokens[paramScan - 1].endPos;
            declarations.push_back({fdName, currentPrefix + fdName, "funcdef", fullDecl, allTokens[nextIdx].startPos, endP});
            idxScan = (paramScan < allTokens.size()) ? paramScan + 1 : paramScan;
            return;
        }
    }
    idxScan++;
}

void HoverHandler::ProcessStatementRule(size_t &idxScan)
{
    if (allTokens[idxScan].text == "for")
    {
        ProcessForLoopRule(idxScan);
    }
    else if (allTokens[idxScan].text == "foreach")
    {
        ProcessForeachLoopRule(idxScan);
    }
    else
    {
        idxScan++;
    }
}

void HoverHandler::ProcessForLoopRule(size_t &idxScan)
{
    size_t loopIdx;
    size_t nextIdx;
    std::string loopTypeStr;
    std::string loopVarName;
    size_t nameTokenIdx;
    std::string deduced;
    size_t activeFuncStart;
    size_t activeFuncEnd;
    size_t entPos;
    std::vector<ScopeRange>::const_iterator scopeIt;

    loopIdx = idxScan + 1;
    if (loopIdx < allTokens.size() && allTokens[loopIdx].text == "(")
    {
        nextIdx = loopIdx + 1;
        loopTypeStr = "";
        if (ParseType(loopIdx + 1, nextIdx, loopTypeStr))
        {
            if (nextIdx < allTokens.size() && IsWord(allTokens[nextIdx].text) && !reservedKeywords.count(allTokens[nextIdx].text))
            {
                loopVarName = allTokens[nextIdx].text;
                nameTokenIdx = nextIdx;

                if (IsAutoDeducibleType(loopTypeStr))
                {
                    if (nameTokenIdx + 1 < allTokens.size() && allTokens[nameTokenIdx + 1].text == "=")
                    {
                        deduced = DeduceTypeFromRHS(nameTokenIdx + 2);
                        if (deduced != "auto")
                        {
                            loopTypeStr = deduced;
                        }
                    }
                }

                activeFuncStart = std::string::npos;
                activeFuncEnd = std::string::npos;
                entPos = allTokens[nameTokenIdx].startPos;
                for (scopeIt = structuralScopes.begin(); scopeIt != structuralScopes.end(); ++scopeIt)
                {
                    if (entPos >= scopeIt->startPos && entPos <= scopeIt->endPos && scopeIt->type == "function")
                    {
                        if (activeFuncStart == std::string::npos || scopeIt->startPos > activeFuncStart)
                        {
                            activeFuncStart = scopeIt->startPos;
                            activeFuncEnd = scopeIt->endPos;
                        }
                    }
                }

                if (activeFuncStart != std::string::npos)
                {
                    declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[nameTokenIdx].startPos, activeFuncEnd});
                }
            }
        }
    }
    idxScan++;
}

void HoverHandler::ProcessForeachLoopRule(size_t &idxScan)
{
    size_t loopIdx;
    size_t nextIdx;
    std::string loopTypeStr;
    std::string loopVarName;
    size_t nameTokenIdx;
    std::string containerName;
    std::vector<DeclInfo>::const_reverse_iterator it;
    size_t lastSpace;
    std::string containerType;
    size_t startAngle;
    size_t endAngle;
    size_t activeFuncStart;
    size_t activeFuncEnd;
    size_t entPos;
    std::vector<ScopeRange>::const_iterator scopeIt;

    loopIdx = idxScan + 1;
    if (loopIdx < allTokens.size() && allTokens[loopIdx].text == "(")
    {
        nextIdx = loopIdx + 1;
        loopTypeStr = "";
        if (ParseType(loopIdx + 1, nextIdx, loopTypeStr))
        {
            if (nextIdx < allTokens.size() && IsWord(allTokens[nextIdx].text) && !reservedKeywords.count(allTokens[nextIdx].text))
            {
                loopVarName = allTokens[nextIdx].text;
                nameTokenIdx = nextIdx;

                containerName = "";
                if (nextIdx + 1 < allTokens.size() && allTokens[nextIdx + 1].text == "in")
                {
                    if (nextIdx + 2 < allTokens.size() && IsWord(allTokens[nextIdx + 2].text))
                    {
                        containerName = allTokens[nextIdx + 2].text;
                    }
                }

                if (IsAutoDeducibleType(loopTypeStr) && !containerName.empty())
                {
                    for (it = declarations.rbegin(); it != declarations.rend(); ++it)
                    {
                        if (it->name == containerName)
                        {
                            lastSpace = it->hoverText.rfind(' ');
                            if (lastSpace != std::string::npos)
                            {
                                containerType = it->hoverText.substr(0, lastSpace);
                                startAngle = containerType.find('<');
                                endAngle = containerType.rfind('>');
                                if (startAngle != std::string::npos && endAngle != std::string::npos && endAngle > startAngle)
                                {
                                    loopTypeStr = containerType.substr(startAngle + 1, endAngle - startAngle - 1);
                                }
                            }
                            break;
                        }
                    }
                }

                activeFuncStart = std::string::npos;
                activeFuncEnd = std::string::npos;
                entPos = allTokens[nameTokenIdx].startPos;
                for (scopeIt = structuralScopes.begin(); scopeIt != structuralScopes.end(); ++scopeIt)
                {
                    if (entPos >= scopeIt->startPos && entPos <= scopeIt->endPos && scopeIt->type == "function")
                    {
                        if (activeFuncStart == std::string::npos || scopeIt->startPos > activeFuncStart)
                        {
                            activeFuncStart = scopeIt->startPos;
                            activeFuncEnd = scopeIt->endPos;
                        }
                    }
                }

                if (activeFuncStart != std::string::npos)
                {
                    declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[nameTokenIdx].startPos, activeFuncEnd});
                }
            }
        }
    }
    idxScan++;
}

void HoverHandler::ProcessScriptRule()
{
    size_t idxScan;
    std::string txt;
    std::string currentPrefix;
    size_t nextIdx;
    std::string typeStr;
    std::string baseType;
    size_t innerScan;
    size_t nameIdx;
    std::string entityName;
    std::string activeFuncName;
    size_t activeFuncStart;
    size_t activeFuncEnd;
    std::string activeClassName;
    size_t activeClassStart;
    size_t activeClassEnd;
    size_t entPos;
    std::vector<ScopeRange>::const_iterator scopeIt;
    size_t closeParen;
    int pDepth;
    size_t modScan;
    std::string modifiers;
    bool isFunctionDef;
    std::vector<TokenInfo> pToks;
    size_t kLoop;
    TokenInfo nTok;
    std::string ptStr;
    size_t tIndex;
    size_t functionEndPos;
    std::vector<ScopeRange>::const_iterator funcScopeIt;
    std::string paramsStr;
    size_t pIndex;
    std::string finalTypeStr;
    std::string deduced;
    size_t searchComma;
    int sqDepth;
    std::string sTxt;
    size_t innerProp;
    bool statementMatched;

    struct FuncParam
    {
        std::string pName;
        std::string pType;
    };
    std::vector<FuncParam> funcParams;

    idxScan = 0;
    while (idxScan < allTokens.size())
    {
        txt = allTokens[idxScan].text;
        currentPrefix = tokenScopePrefixes[idxScan];

        if (IsStructureDeclarationKeyword(txt))
        {
            idxScan++;
            continue;
        }
        if (IsStorageModifierKeyword(txt))
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

        if (txt == "foreach" || txt == "for")
        {
            ProcessStatementRule(idxScan);
            continue;
        }

        if (txt == "funcdef")
        {
            ProcessFuncDefRule(idxScan, currentPrefix);
            continue;
        }

        nextIdx = idxScan;
        typeStr = "";
        if (ParseType(idxScan, nextIdx, typeStr))
        {
            if (nextIdx <= idxScan)
            {
                idxScan++;
                continue;
            }
            baseType = allTokens[idxScan].text;

            if (IsStatementKeyword(baseType))
            {
                idxScan++;
                continue;
            }

            statementMatched = false;
            innerScan = nextIdx;

            while (innerScan < allTokens.size() && IsWord(allTokens[innerScan].text) && !reservedKeywords.count(allTokens[innerScan].text))
            {
                nameIdx = innerScan;
                entityName = allTokens[nameIdx].text;
                activeFuncName = "";
                activeFuncStart = std::string::npos;
                activeFuncEnd = std::string::npos;
                activeClassName = "";
                activeClassStart = std::string::npos;
                activeClassEnd = std::string::npos;
                entPos = allTokens[nameIdx].startPos;

                for (scopeIt = structuralScopes.begin(); scopeIt != structuralScopes.end(); ++scopeIt)
                {
                    if (entPos >= scopeIt->startPos && entPos <= scopeIt->endPos)
                    {
                        if (scopeIt->type == "function")
                        {
                            if (activeFuncStart == std::string::npos || scopeIt->startPos > activeFuncStart)
                            {
                                activeFuncName = scopeIt->name;
                                activeFuncStart = scopeIt->startPos;
                                activeFuncEnd = scopeIt->endPos;
                            }
                        }
                        else if (scopeIt->type == "class" || scopeIt->type == "interface" || scopeIt->type == "mixin class" || scopeIt->type == "abstract class")
                        {
                            if (activeClassStart == std::string::npos || scopeIt->startPos > activeClassStart)
                            {
                                activeClassName = scopeIt->name;
                                activeClassStart = scopeIt->startPos;
                                activeClassEnd = scopeIt->endPos;
                            }
                        }
                    }
                }

                if (nameIdx + 1 < allTokens.size() && allTokens[nameIdx + 1].text == "(")
                {
                    closeParen = nameIdx + 1;
                    pDepth = 1;
                    while (closeParen + 1 < allTokens.size() && pDepth > 0)
                    {
                        closeParen++;
                        if (allTokens[closeParen].text == "(")
                            pDepth++;
                        else if (allTokens[closeParen].text == ")")
                            pDepth--;
                    }

                    modScan = closeParen + 1;
                    modifiers = "";
                    while (modScan < allTokens.size() && allTokens[modScan].text != ";" && allTokens[modScan].text != "{" && allTokens[modScan].text != ",")
                    {
                        if (contextualKeywords.count(allTokens[modScan].text) || reservedKeywords.count(allTokens[modScan].text))
                            modifiers += " " + allTokens[modScan].text;
                        modScan++;
                    }

                    isFunctionDef = (modScan < allTokens.size() && allTokens[modScan].text == "{") ||
                                    (activeClassStart != std::string::npos && activeFuncStart == std::string::npos) ||
                                    (activeClassStart == std::string::npos && activeFuncStart == std::string::npos && modifiers.find("import") != std::string::npos);

                    if (isFunctionDef)
                    {
                        funcParams.clear();
                        pToks.clear();

                        for (kLoop = nameIdx + 2; kLoop < closeParen; ++kLoop)
                        {
                            if (allTokens[kLoop].text == "," || kLoop == closeParen - 1)
                            {
                                if (kLoop == closeParen - 1 && allTokens[kLoop].text != ",")
                                    pToks.push_back(allTokens[kLoop]);
                                if (!pToks.empty() && IsWord(pToks.back().text) && !reservedKeywords.count(pToks.back().text))
                                {
                                    nTok = pToks.back();
                                    pToks.pop_back();
                                    ptStr = "";
                                    for (tIndex = 0; tIndex < pToks.size(); ++tIndex)
                                    {
                                        if (tIndex > 0 && pToks[tIndex].text != "@" && pToks[tIndex].text != "&" && pToks[tIndex - 1].text != "::")
                                            ptStr += " ";
                                        ptStr += pToks[tIndex].text;
                                    }
                                    funcParams.push_back({nTok.text, ptStr});
                                }
                                pToks.clear();
                            }
                            else
                                pToks.push_back(allTokens[kLoop]);
                        }

                        functionEndPos = allTokens[closeParen].endPos;
                        if (modScan < allTokens.size() && allTokens[modScan].text == "{")
                        {
                            for (funcScopeIt = structuralScopes.begin(); funcScopeIt != structuralScopes.end(); ++funcScopeIt)
                            {
                                if (funcScopeIt->type == "function" && funcScopeIt->name == entityName && funcScopeIt->startPos == allTokens[modScan].startPos)
                                {
                                    functionEndPos = funcScopeIt->endPos;
                                    break;
                                }
                            }
                        }

                        paramsStr = "";
                        for (pIndex = 0; pIndex < funcParams.size(); ++pIndex)
                        {
                            if (pIndex > 0)
                                paramsStr += ", ";
                            paramsStr += funcParams[pIndex].pType + " " + funcParams[pIndex].pName;
                        }

                        declarations.push_back({entityName, currentPrefix + entityName, "function", typeStr + " " + currentPrefix + entityName + "(" + paramsStr + ")" + modifiers, allTokens[nameIdx].startPos, functionEndPos});

                        if (functionEndPos != std::string::npos)
                        {
                            for (const auto &fp : funcParams)
                                declarations.push_back({fp.pName, fp.pName, "parameter", fp.pType + " " + fp.pName, allTokens[nameIdx].startPos, functionEndPos});
                        }

                        idxScan = (modScan < allTokens.size() && allTokens[modScan].text == "{") ? modScan + 1 : modScan;
                        statementMatched = true;
                        break;
                    }
                    else
                    {
                        finalTypeStr = typeStr;
                        if (IsAutoDeducibleType(typeStr))
                        {
                            if (allTokens[nameIdx + 1].text == "=" && nameIdx + 2 < allTokens.size())
                            {
                                deduced = DeduceTypeFromRHS(nameIdx + 2);
                                if (deduced != "auto")
                                {
                                    finalTypeStr = deduced;
                                    if (typeStr.find('@') != std::string::npos && finalTypeStr.find('@') == std::string::npos)
                                        finalTypeStr += "@";
                                }
                            }
                        }

                        if (activeFuncStart != std::string::npos)
                            declarations.push_back({entityName, entityName, "local_variable", finalTypeStr + " " + entityName, activeFuncStart, activeFuncEnd});
                        else if (activeClassStart != std::string::npos)
                            declarations.push_back({entityName, currentPrefix + entityName, "property", finalTypeStr + " " + currentPrefix + entityName, activeClassStart, activeClassEnd});
                        else
                            declarations.push_back({entityName, currentPrefix + entityName, "global_variable", finalTypeStr + " " + currentPrefix + entityName, allTokens[nameIdx].startPos, std::string::npos});

                        searchComma = closeParen + 1;
                        sqDepth = 0;
                        while (searchComma < allTokens.size())
                        {
                            sTxt = allTokens[searchComma].text;
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
                    modifiers = " property";
                    innerProp = nameIdx + 2;
                    while (innerProp < allTokens.size() && allTokens[innerProp].text != "}")
                    {
                        if (allTokens[innerProp].text == "const" || allTokens[innerProp].text == "override")
                            modifiers += " " + allTokens[innerProp].text;
                        innerProp++;
                    }

                    declarations.push_back({entityName, currentPrefix + entityName, "property", typeStr + " " + currentPrefix + entityName + modifiers, allTokens[nameIdx].startPos, allTokens[innerProp].endPos});
                    idxScan = innerProp + 1;
                    statementMatched = true;
                    break;
                }
                else if (nameIdx + 1 < allTokens.size() && (allTokens[nameIdx + 1].text == ";" || allTokens[nameIdx + 1].text == "=" || allTokens[nameIdx + 1].text == "," || allTokens[nameIdx + 1].text == "[" || allTokens[nameIdx + 1].text == ")"))
                {
                    finalTypeStr = typeStr;
                    if (IsAutoDeducibleType(typeStr))
                    {
                        if (allTokens[nameIdx + 1].text == "=" && nameIdx + 2 < allTokens.size())
                        {
                            deduced = DeduceTypeFromRHS(nameIdx + 2);
                            if (deduced != "auto")
                            {
                                finalTypeStr = deduced;
                                if (typeStr.find('@') != std::string::npos && finalTypeStr.find('@') == std::string::npos)
                                    finalTypeStr += "@";
                            }
                        }
                    }

                    if (activeFuncStart != std::string::npos)
                        declarations.push_back({entityName, entityName, "local_variable", finalTypeStr + " " + entityName, activeFuncStart, activeFuncEnd});
                    else if (activeClassStart != std::string::npos)
                        declarations.push_back({entityName, currentPrefix + entityName, "property", finalTypeStr + " " + currentPrefix + entityName, activeClassStart, activeClassEnd});
                    else
                        declarations.push_back({entityName, currentPrefix + entityName, "global_variable", finalTypeStr + " " + currentPrefix + entityName, allTokens[nameIdx].startPos, std::string::npos});

                    searchComma = nameIdx + 1;
                    sqDepth = 0;
                    while (searchComma < allTokens.size())
                    {
                        sTxt = allTokens[searchComma].text;
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
                continue;
            idxScan = nextIdx;
            continue;
        }
        idxScan++;
    }
}

void HoverHandler::ProcessLambdaRule(size_t &k)
{
    size_t nameIdx;
    size_t closeParen;
    int pDepth;
    size_t idxP;
    std::vector<TokenInfo> pToks;
    TokenInfo nTok;
    std::string ptStr;
    size_t t;
    std::string inferredRet;
    int lookBack;
    std::string targetVar;
    std::vector<DeclInfo>::const_reverse_iterator it;
    size_t spacePos;
    size_t modScan;
    size_t bodyBraceIdx;
    size_t functionEndPos;
    std::vector<ScopeRange>::const_iterator scopeIt;
    std::string paramsStr;
    size_t p;
    std::string hoverTxt;
    size_t lambdaBraceDepth;
    size_t scanIdx;
    size_t endBraceTokenIdx;
    size_t lambdaIdxScan;
    std::string lTxt;
    size_t loopIdx;
    size_t nextIdxLoop;
    std::string loopTypeStr;
    std::string loopVarName;
    std::string containerName;
    size_t lastSpaceLoop;
    std::string containerType;
    size_t startAngle;
    size_t endAngle;
    size_t nameTokenIdx;
    std::string deducedFor;
    std::string typeStr;
    size_t nextIdx;
    std::string baseType;
    size_t innerScan;
    bool advanced;
    size_t nameIdxLocal;
    std::string entityName;
    std::string finalTypeStr;
    std::string deducedLocal;
    size_t searchComma;
    int sqDepth;
    std::string sTxt;
    std::string_view accessStrippedView;

    struct FuncParam
    {
        std::string pName;
        std::string pType;
    };
    std::vector<FuncParam> funcParams;

    if (allTokens[k].text == "function" && k + 1 < allTokens.size() && allTokens[k + 1].text == "(")
    {
        nameIdx = k;
        closeParen = nameIdx + 1;
        pDepth = 1;

        while (closeParen + 1 < allTokens.size() && pDepth > 0)
        {
            closeParen++;
            if (allTokens[closeParen].text == "(")
                pDepth++;
            else if (allTokens[closeParen].text == ")")
                pDepth--;
        }

        pToks.clear();
        funcParams.clear();

        for (idxP = nameIdx + 2; idxP < closeParen; ++idxP)
        {
            if (allTokens[idxP].text == "," || idxP == closeParen - 1)
            {
                if (idxP == closeParen - 1 && allTokens[idxP].text != ",")
                    pToks.push_back(allTokens[idxP]);
                if (!pToks.empty() && IsWord(pToks.back().text) && !reservedKeywords.count(pToks.back().text))
                {
                    nTok = pToks.back();
                    pToks.pop_back();
                    ptStr = "";
                    for (t = 0; t < pToks.size(); ++t)
                    {
                        if (t > 0 && pToks[t].text != "@" && pToks[t].text != "&" && pToks[t - 1].text != "::")
                            ptStr += " ";
                        ptStr += pToks[t].text;
                    }
                    funcParams.push_back({nTok.text, ptStr});
                }
                pToks.clear();
            }
            else
                pToks.push_back(allTokens[idxP]);
        }

        inferredRet = "auto";
        lookBack = static_cast<int>(nameIdx) - 1;

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
            targetVar = allTokens[lookBack].text;
            for (it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                if (it->name == targetVar && (it->type == "local_variable" || it->type == "property" || it->type == "global_variable" || it->type == "parameter"))
                {
                    spacePos = it->hoverText.rfind(' ');
                    if (spacePos != std::string::npos)
                    {
                        accessStrippedView = StripAccessModifiers(std::string_view(it->hoverText).substr(0, spacePos));
                        inferredRet = std::string(accessStrippedView);

                        while (!inferredRet.empty() && (inferredRet.back() == '@' || inferredRet.back() == '&'))
                            inferredRet.pop_back();
                    }
                    break;
                }
            }
        }

        modScan = closeParen + 1;
        bodyBraceIdx = std::string::npos;
        if (modScan < allTokens.size() && allTokens[modScan].text == "{")
            bodyBraceIdx = modScan;

        functionEndPos = allTokens[closeParen].endPos;
        if (bodyBraceIdx != std::string::npos)
        {
            for (scopeIt = structuralScopes.begin(); scopeIt != structuralScopes.end(); ++scopeIt)
            {
                if (scopeIt->type == "function" && scopeIt->startPos == allTokens[bodyBraceIdx].startPos)
                {
                    functionEndPos = scopeIt->endPos;
                    break;
                }
            }
        }

        paramsStr = "";
        for (p = 0; p < funcParams.size(); ++p)
        {
            if (p > 0)
                paramsStr += ", ";
            paramsStr += funcParams[p].pType + " " + funcParams[p].pName;
        }

        hoverTxt = inferredRet + " function(" + paramsStr + ")";
        declarations.push_back({"function", "function", "lambda", hoverTxt, allTokens[nameIdx].startPos, functionEndPos});

        for (const auto &fp : funcParams)
            declarations.push_back({fp.pName, fp.pName, "parameter", fp.pType + " " + fp.pName, allTokens[nameIdx].startPos, functionEndPos});

        if (bodyBraceIdx != std::string::npos)
        {
            lambdaBraceDepth = 1;
            scanIdx = bodyBraceIdx + 1;
            endBraceTokenIdx = bodyBraceIdx;

            while (scanIdx < allTokens.size() && lambdaBraceDepth > 0)
            {
                if (allTokens[scanIdx].text == "{")
                    lambdaBraceDepth++;
                else if (allTokens[scanIdx].text == "}")
                    lambdaBraceDepth--;

                if (lambdaBraceDepth == 0)
                {
                    endBraceTokenIdx = scanIdx;
                    break;
                }
                scanIdx++;
            }

            lambdaIdxScan = bodyBraceIdx + 1;
            while (lambdaIdxScan < endBraceTokenIdx)
            {
                lTxt = allTokens[lambdaIdxScan].text;

                if (lTxt == "foreach")
                {
                    loopIdx = lambdaIdxScan + 1;
                    if (loopIdx < endBraceTokenIdx && allTokens[loopIdx].text == "(")
                    {
                        nextIdxLoop = loopIdx + 1;
                        loopTypeStr = "";
                        if (ParseType(loopIdx + 1, nextIdxLoop, loopTypeStr))
                        {
                            if (nextIdxLoop < endBraceTokenIdx && IsWord(allTokens[nextIdxLoop].text) && !reservedKeywords.count(allTokens[nextIdxLoop].text))
                            {
                                loopVarName = allTokens[nextIdxLoop].text;
                                containerName = "";
                                if (nextIdxLoop + 1 < endBraceTokenIdx && allTokens[nextIdxLoop + 1].text == "in")
                                {
                                    if (nextIdxLoop + 2 < endBraceTokenIdx && IsWord(allTokens[nextIdxLoop + 2].text))
                                    {
                                        containerName = allTokens[nextIdxLoop + 2].text;
                                    }
                                }

                                if (IsAutoDeducibleType(loopTypeStr) && !containerName.empty())
                                {
                                    for (it = declarations.rbegin(); it != declarations.rend(); ++it)
                                    {
                                        if (it->name == containerName)
                                        {
                                            lastSpaceLoop = it->hoverText.rfind(' ');
                                            if (lastSpaceLoop != std::string::npos)
                                            {
                                                containerType = it->hoverText.substr(0, lastSpaceLoop);
                                                startAngle = containerType.find('<');
                                                endAngle = containerType.rfind('>');
                                                if (startAngle != std::string::npos && endAngle != std::string::npos && endAngle > startAngle)
                                                {
                                                    loopTypeStr = containerType.substr(startAngle + 1, endAngle - startAngle - 1);
                                                }
                                            }
                                            break;
                                        }
                                    }
                                }
                                declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[loopIdx].startPos, functionEndPos});
                            }
                        }
                    }
                    lambdaIdxScan++;
                    continue;
                }

                if (lTxt == "for")
                {
                    loopIdx = lambdaIdxScan + 1;
                    if (loopIdx < endBraceTokenIdx && allTokens[loopIdx].text == "(")
                    {
                        nextIdxLoop = loopIdx + 1;
                        loopTypeStr = "";
                        if (ParseType(loopIdx + 1, nextIdxLoop, loopTypeStr))
                        {
                            if (nextIdxLoop < endBraceTokenIdx && IsWord(allTokens[nextIdxLoop].text) && !reservedKeywords.count(allTokens[nextIdxLoop].text))
                            {
                                loopVarName = allTokens[nextIdxLoop].text;
                                nameTokenIdx = nextIdxLoop;

                                if (IsAutoDeducibleType(loopTypeStr))
                                {
                                    if (nameTokenIdx + 1 < endBraceTokenIdx && allTokens[nameTokenIdx + 1].text == "=")
                                    {
                                        deducedFor = DeduceTypeFromRHS(nameTokenIdx + 2);
                                        if (deducedFor != "auto")
                                        {
                                            loopTypeStr = deducedFor;
                                        }
                                    }
                                }
                                declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[nameTokenIdx].startPos, functionEndPos});
                            }
                        }
                    }
                    lambdaIdxScan++;
                    continue;
                }

                typeStr = "";
                nextIdx = lambdaIdxScan;

                if (ParseType(lambdaIdxScan, nextIdx, typeStr))
                {
                    if (nextIdx <= lambdaIdxScan)
                    {
                        lambdaIdxScan++;
                        continue;
                    }
                    baseType = allTokens[lambdaIdxScan].text;

                    if (IsStatementKeyword(baseType))
                    {
                        lambdaIdxScan = nextIdx;
                        continue;
                    }

                    innerScan = nextIdx;
                    advanced = false;
                    while (innerScan < endBraceTokenIdx && IsWord(allTokens[innerScan].text) && !reservedKeywords.count(allTokens[innerScan].text))
                    {
                        nameIdxLocal = innerScan;
                        entityName = allTokens[nameIdxLocal].text;

                        if (nameIdxLocal + 1 < allTokens.size() && (allTokens[nameIdxLocal + 1].text == ";" || allTokens[nameIdxLocal + 1].text == "=" || allTokens[nameIdxLocal + 1].text == "," || allTokens[nameIdxLocal + 1].text == "[" || allTokens[nameIdxLocal + 1].text == ")"))
                        {
                            finalTypeStr = typeStr;
                            if (IsAutoDeducibleType(typeStr))
                            {
                                if (allTokens[nameIdxLocal + 1].text == "=" && nameIdxLocal + 2 < allTokens.size())
                                {
                                    deducedLocal = DeduceTypeFromRHS(nameIdxLocal + 2);
                                    if (deducedLocal != "auto")
                                    {
                                        finalTypeStr = deducedLocal;
                                        if (typeStr.find('@') != std::string::npos && finalTypeStr.find('@') == std::string::npos)
                                            finalTypeStr += "@";
                                    }
                                }
                            }

                            declarations.push_back({entityName, entityName, "local_variable", finalTypeStr + " " + entityName, allTokens[bodyBraceIdx].startPos, functionEndPos});

                            searchComma = nameIdxLocal + 1;
                            sqDepth = 0;
                            while (searchComma < endBraceTokenIdx)
                            {
                                sTxt = allTokens[searchComma].text;
                                if (sTxt == "(" || sTxt == "[" || sTxt == "{")
                                    sqDepth++;
                                else if (sTxt == ")" || sTxt == "]" || sTxt == "}")
                                    sqDepth--;
                                else if (sqDepth == 0 && (sTxt == "," || sTxt == ";"))
                                    break;
                                searchComma++;
                            }

                            lambdaIdxScan = (searchComma < endBraceTokenIdx) ? searchComma + 1 : searchComma;
                            advanced = true;
                            break;
                        }
                        innerScan++;
                    }

                    if (advanced)
                        continue;

                    lambdaIdxScan = nextIdx;
                    continue;
                }
                lambdaIdxScan++;
            }
        }
        k = closeParen;
    }
}

std::string HoverHandler::DeduceTypeFromRHS(size_t startIdx)
{
    size_t curIdx;
    std::string firstToken;
    std::vector<DeclInfo>::const_reverse_iterator it;
    size_t lastSpace;
    std::string deduced;
    size_t startAngle;
    size_t endAngle;

    if (startIdx >= allTokens.size())
        return "auto";

    curIdx = startIdx;
    if (allTokens[curIdx].text == "@")
    {
        curIdx++;
    }

    if (curIdx >= allTokens.size())
        return "auto";

    if (allTokens[curIdx].tokenClass == asTC_VALUE)
    {
        if (!allTokens[curIdx].text.empty() && allTokens[curIdx].text[0] == '"')
        {
            return "string";
        }
        if (!allTokens[curIdx].text.empty() && std::isdigit(static_cast<unsigned char>(allTokens[curIdx].text[0])))
        {
            if (allTokens[curIdx].text.find('.') != std::string::npos || allTokens[curIdx].text.back() == 'f')
            {
                return "float";
            }
            return "int";
        }
    }

    firstToken = allTokens[curIdx].text;

    if (IsPrimitiveType(firstToken))
    {
        return firstToken;
    }

    for (it = declarations.rbegin(); it != declarations.rend(); ++it)
    {
        if (it->name == firstToken || (it->fullName.length() >= firstToken.length() &&
                                       it->fullName.substr(it->fullName.length() - firstToken.length()) == firstToken))
        {
            lastSpace = it->hoverText.rfind(' ');
            if (lastSpace != std::string::npos)
            {
                deduced = it->hoverText.substr(0, lastSpace);

                if (curIdx + 1 < allTokens.size() && allTokens[curIdx + 1].text == "[")
                {
                    startAngle = deduced.find('<');
                    endAngle = deduced.rfind('>');
                    if (startAngle != std::string::npos && endAngle != std::string::npos && endAngle > startAngle)
                    {
                        deduced = deduced.substr(startAngle + 1, endAngle - startAngle - 1);
                    }
                }
                return deduced;
            }
        }
    }

    if (nativeEng)
    {
        if (nativeEng->GetTypeInfoByName(firstToken.c_str()))
        {
            return firstToken;
        }
    }
    return "auto";
}

std::string HoverHandler::EnhanceIfFuncdef(const std::string &hoverText)
{
    std::string baseHover;
    std::string_view cleanedView;
    size_t firstSpace;
    std::string typeName;
    std::string paramsSuffix;
    std::vector<DeclInfo>::const_iterator dIt;
    size_t paren;
    asIScriptModule *mod;
    asITypeInfo *fdefInfo;
    asIScriptFunction *fdefFunc;
    std::string fullFuncDecl;
    size_t parenPos;

    baseHover = hoverText;
    if (!nativeEng)
        return baseHover;

    cleanedView = StripAccessModifiers(baseHover);
    firstSpace = cleanedView.find(' ');
    if (firstSpace != std::string_view::npos)
    {
        typeName = std::string(cleanedView.substr(0, firstSpace));
        if (!typeName.empty() && typeName.back() == '@')
            typeName.pop_back();

        paramsSuffix = "";

        for (dIt = declarations.begin(); dIt != declarations.end(); ++dIt)
        {
            if (dIt->type == "funcdef" && dIt->name == typeName)
            {
                paren = dIt->hoverText.find('(');
                if (paren != std::string::npos)
                {
                    paramsSuffix = dIt->hoverText.substr(paren);
                    break;
                }
            }
        }

        if (paramsSuffix.empty())
        {
            mod = nativeEng->GetModule("LSPModule");
            fdefInfo = mod ? mod->GetTypeInfoByName(typeName.c_str()) : nullptr;
            if (!fdefInfo)
            {
                fdefInfo = nativeEng->GetTypeInfoByName(typeName.c_str());
            }

            if (fdefInfo && (fdefInfo->GetFlags() & asOBJ_FUNCDEF))
            {
                fdefFunc = fdefInfo->GetFuncdefSignature();
                if (fdefFunc)
                {
                    fullFuncDecl = fdefFunc->GetDeclaration(true, true);
                    parenPos = fullFuncDecl.find('(');
                    if (parenPos != std::string::npos)
                    {
                        paramsSuffix = fullFuncDecl.substr(parenPos);
                    }
                }
            }
        }

        if (!paramsSuffix.empty())
        {
            baseHover += paramsSuffix;
        }
    }
    return baseHover;
}

bool HoverHandler::MatchesQual(const DeclInfo &decl, const std::string &fullQualName) const
{
    size_t p;
    if (decl.fullName == fullQualName)
        return true;
    if (decl.fullName.length() > fullQualName.length())
    {
        p = decl.fullName.rfind(fullQualName);
        if (p != std::string::npos && p + fullQualName.length() == decl.fullName.length())
        {
            if (p >= 2 && decl.fullName.substr(p - 2, 2) == "::")
                return true;
        }
    }
    return false;
}

std::string HoverHandler::SemanticValidationPass(int targetIdx)
{
    std::string hoverResult;
    TokenInfo targetTok;
    bool isMemberAccess;
    std::string objName;
    int backIdx;
    int depth;
    std::string objType;
    std::vector<DeclInfo>::const_reverse_iterator it;
    size_t lastSpace;
    std::string cleanTypeName;
    std::vector<std::string> searchHierarchy;
    bool methodResolved;
    std::vector<std::string>::const_iterator typeIt;
    asITypeInfo *typeInfo;
    asUINT m;
    asIScriptFunction *methodFunc;
    std::string declStr;
    size_t tppos;
    size_t namePos;
    size_t openBracket;
    size_t closeBracket;
    std::string templateArg;
    size_t tPos;
    bool leftOk;
    bool rightOk;
    std::string fullQualName;
    int left;
    bool isFollowedByParen;
    bool isPrecededByTilde;
    asUINT f;
    asITypeInfo *fInfo;
    asIScriptFunction *fFunc;
    asITypeInfo *tInfo;
    std::string_view accessStrippedView;

    hoverResult = "";

    if (targetIdx == -1)
        return "";

    targetTok = allTokens[targetIdx];
    isMemberAccess = (targetIdx >= 1 && allTokens[targetIdx - 1].text == ".");

    if (isMemberAccess && targetTok.tokenClass == asTC_IDENTIFIER)
    {
        objName = "";
        backIdx = targetIdx - 2;

        if (backIdx >= 0 && allTokens[backIdx].text == "]")
        {
            depth = 1;
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
            objName = allTokens[backIdx].text;

        objType = "";
        for (it = declarations.rbegin(); it != declarations.rend(); ++it)
        {
            if (it->name == objName && (it->type == "local_variable" || it->type == "parameter" || it->type == "property" || it->type == "global_variable"))
            {
                if (it->type != "local_variable" && it->type != "parameter" || (targetTok.startPos >= it->startPos && targetTok.startPos <= it->endPos))
                {
                    lastSpace = it->hoverText.rfind(' ');
                    if (lastSpace != std::string::npos)
                    {
                        accessStrippedView = StripAccessModifiers(std::string_view(it->hoverText).substr(0, lastSpace));
                        objType = std::string(accessStrippedView);
                    }
                    break;
                }
            }
        }

        if (!objType.empty())
        {
            cleanTypeName = ExtractBaseTypeName(objType);
            searchHierarchy = {cleanTypeName};
            if (classInheritanceMapper.count(cleanTypeName))
            {
                for (const auto &baseClass : classInheritanceMapper[cleanTypeName])
                    searchHierarchy.push_back(baseClass);
            }

            methodResolved = false;
            for (typeIt = searchHierarchy.begin(); typeIt != searchHierarchy.end(); ++typeIt)
            {
                for (it = declarations.rbegin(); it != declarations.rend(); ++it)
                {
                    if (it->name == targetTok.text && IsCallableOrFieldSemantic(it->type))
                    {
                        if (it->fullName.find(*typeIt + "::") != std::string::npos)
                        {
                            hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(EnhanceIfFuncdef(it->hoverText)));
                            methodResolved = true;
                            break;
                        }
                    }
                }

                if (methodResolved)
                    break;

                if (nativeEng)
                {
                    typeInfo = nativeEng->GetTypeInfoByName(typeIt->c_str());
                    if (typeInfo)
                    {
                        for (m = 0; m < typeInfo->GetMethodCount(); ++m)
                        {
                            methodFunc = typeInfo->GetMethodByIndex(m);
                            if (methodFunc && methodFunc->GetName() == targetTok.text)
                            {
                                declStr = methodFunc->GetDeclaration(true, true);
                                tppos = declStr.find("T[]::");
                                if (tppos != std::string::npos)
                                    declStr.erase(tppos, 5);

                                if (declStr.find(cleanTypeName + "::") == std::string::npos)
                                {
                                    namePos = declStr.find(targetTok.text + "(");
                                    if (namePos != std::string::npos)
                                        declStr.insert(namePos, objType + "::");
                                }

                                openBracket = objType.find('<');
                                closeBracket = objType.rfind('>');
                                if (openBracket != std::string::npos && closeBracket != std::string::npos && closeBracket > openBracket)
                                {
                                    templateArg = objType.substr(openBracket + 1, closeBracket - openBracket - 1);
                                    tPos = 0;
                                    while ((tPos = declStr.find('T', tPos)) != std::string::npos)
                                    {
                                        leftOk = (tPos == 0 || (!std::isalnum(static_cast<unsigned char>(declStr[tPos - 1])) && declStr[tPos - 1] != '_'));
                                        rightOk = (tPos + 1 == declStr.length() || (!std::isalnum(static_cast<unsigned char>(declStr[tPos + 1])) && declStr[tPos + 1] != '_'));

                                        if (leftOk && rightOk)
                                        {
                                            declStr.replace(tPos, 1, templateArg);
                                            tPos += templateArg.length();
                                        }
                                        else
                                        {
                                            tPos++;
                                        }
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
                    break;
            }
        }

        if (hoverResult.empty())
            return "";
    }

    if (hoverResult.empty() && (reservedKeywords.count(targetTok.text) || contextualKeywords.count(targetTok.text)) && !primitiveTypes.count(targetTok.text))
    {
        if (!IsKeywordHoverException(targetTok.text))
            return "";
    }

    if (hoverResult.empty() && targetTok.tokenClass == asTC_VALUE)
    {
        if (!targetTok.text.empty() && targetTok.text[0] == '"')
            hoverResult = fmt::format("```cpp\n(const char [{}]) {}\n```", targetTok.text.length() - 1, targetTok.text);
        else
            hoverResult = fmt::format("```cpp\n(int) {}\n```", targetTok.text);
    }

    if (hoverResult.empty() && targetTok.tokenClass == asTC_IDENTIFIER)
    {
        fullQualName = targetTok.text;
        left = targetIdx - 1;
        while (left >= 0 && allTokens[left].text == "::")
        {
            if (left > 0 && IsWord(allTokens[left - 1].text))
            {
                fullQualName = allTokens[left - 1].text + "::" + fullQualName;
                left -= 2;
            }
            else
                break;
        }

        isFollowedByParen = (targetIdx + 1 < static_cast<int>(allTokens.size()) && allTokens[targetIdx + 1].text == "(");
        isPrecededByTilde = (targetIdx > 0 && allTokens[targetIdx - 1].text == "~");

        for (it = declarations.rbegin(); it != declarations.rend(); ++it)
        {
            if ((it->type == "local_variable" || it->type == "parameter" || it->type == "global_variable") && it->name == targetTok.text)
            {
                if (it->type == "global_variable" || (targetTok.startPos >= it->startPos && targetTok.startPos <= it->endPos))
                {
                    hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(EnhanceIfFuncdef(it->hoverText)));
                    break;
                }
            }
        }

        if (hoverResult.empty() && (isFollowedByParen || isPrecededByTilde || targetTok.text == "function"))
        {
            for (it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                if ((it->type == "function" || it->type == "lambda") && MatchesQual(*it, fullQualName))
                {
                    if (it->type == "lambda")
                    {
                        if (targetTok.startPos == it->startPos)
                        {
                            hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(it->hoverText));
                            break;
                        }
                    }
                    else
                    {
                        hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(it->hoverText));
                        break;
                    }
                }
            }
        }

        if (hoverResult.empty())
        {
            for (it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                if (it->type != "function" && it->type != "local_variable" && it->type != "parameter" && it->type != "global_variable" && it->type != "lambda" && MatchesQual(*it, fullQualName))
                {
                    hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(EnhanceIfFuncdef(it->hoverText)));
                    break;
                }
            }
        }

        if (hoverResult.empty() && nativeEng)
        {
            for (f = 0; f < nativeEng->GetFuncdefCount(); ++f)
            {
                fInfo = nativeEng->GetFuncdefByIndex(f);
                if (fInfo && fInfo->GetName() == targetTok.text)
                {
                    fFunc = fInfo->GetFuncdefSignature();
                    if (fFunc)
                    {
                        hoverResult = fmt::format("```cpp\nfuncdef {}\n```", CleanSignature(fFunc->GetDeclaration(true, true)));
                        break;
                    }
                }
            }

            if (hoverResult.empty())
            {
                tInfo = nativeEng->GetTypeInfoByName(targetTok.text.c_str());
                if (tInfo)
                    hoverResult = fmt::format("```cpp\nclass {}\n```", tInfo->GetName());
            }
        }
    }

    return hoverResult;
}

json HoverHandler::Process()
{
    int targetIdx;
    std::string resultMarkdown;
    size_t k;

    TokenizePass();
    targetIdx = FindTargetTokenIdx();

    StructuralParsingPass();
    ProcessScriptRule();

    for (k = 0; k < allTokens.size(); ++k)
    {
        if (allTokens[k].text == "function")
        {
            ProcessLambdaRule(k);
        }
    }

    resultMarkdown = SemanticValidationPass(targetIdx);

    if (!resultMarkdown.empty())
    {
        return {{"contents", {{"kind", "markdown"}, {"value", resultMarkdown}}}};
    }
    return nullptr;
}

bool HoverHandler::IsStructureDeclarationKeyword(std::string_view txt) const
{
    return txt == "class" || txt == "interface" || txt == "namespace" || txt == "enum" || txt == "mixin" || txt == "abstract";
}

bool HoverHandler::IsStatementKeyword(std::string_view txt) const
{
    return txt == "if" || txt == "for" || txt == "foreach" || txt == "while" || txt == "return" ||
           txt == "break" || txt == "continue" || txt == "switch" || txt == "case" || txt == "default" ||
           txt == "cast" || txt == "try" || txt == "catch" || txt == "delete" || txt == "throw";
}

bool HoverHandler::IsStorageModifierKeyword(std::string_view txt) const
{
    return storageModifiers.count(std::string(txt)) > 0;
}

bool HoverHandler::IsPrimitiveType(std::string_view txt) const
{
    return primitiveTypes.count(std::string(txt)) > 0;
}