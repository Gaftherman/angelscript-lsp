/**
 * @file HoverHandler.cpp
 * @brief Processes the syntactic and semantic context for Language Server Protocol (LSP) hover requests.
 */

#include "HoverHandler.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fmt/core.h>

namespace HoverUtils
{
    size_t ScanInlineChainedDeclarations(const std::vector<HoverHandler::TokenInfo> &allTokens, size_t startIdx, size_t boundaryLimit) noexcept
    {
        size_t index = startIdx;
        int squareBracketDepth = 0;

        while (index < boundaryLimit)
        {
            std::string_view tokenVal = allTokens[index].text;

            if (tokenVal == "(" || tokenVal == "[" || tokenVal == "{")
            {
                squareBracketDepth++;
            }
            else if (tokenVal == ")" || tokenVal == "]" || tokenVal == "}")
            {
                squareBracketDepth--;
            }
            else if (squareBracketDepth == 0 && (tokenVal == "," || tokenVal == ";"))
            {
                break;
            }

            index++;
        }

        return index;
    }
}

const std::unordered_set<std::string_view> HoverHandler::reservedKeywords = {
    "and", "auto", "bool", "break", "case", "cast", "catch", "class", "const", "continue",
    "default", "do", "double", "else", "enum", "false", "float", "for", "foreach", "funcdef",
    "if", "import", "in", "inout", "int", "interface", "int8", "int16", "int32", "int64",
    "is", "mixin", "namespace", "not", "null", "or", "out", "private", "protected", "return",
    "switch", "true", "try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64", "using",
    "void", "while", "xor"};

const std::unordered_set<std::string_view> HoverHandler::contextualKeywords = {
    "abstract", "delete", "explicit", "external", "final", "from", "function", "get",
    "override", "property", "set", "shared", "super", "this"};

const std::unordered_set<std::string_view> HoverHandler::primitiveTypes = {
    "void", "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64", "float", "double", "bool", "auto"};

const std::unordered_set<std::string_view> HoverHandler::storageModifiers = {
    "private", "protected", "public", "shared", "external"};

const std::unordered_set<std::string_view> HoverHandler::structureKeywords = {
    "class", "interface", "namespace", "enum", "mixin", "abstract"};

const std::unordered_set<std::string_view> HoverHandler::statementKeywords = {
    "if", "for", "foreach", "while", "return", "break", "continue", "switch", "case", "default", "cast", "try", "catch", "delete", "throw"};

// =========================================================================
// HIGH-PERFORMANCE ABSTRACTED KEYWORD PREDICATES & SCOPE UTILITIES
// =========================================================================

bool HoverHandler::IsStructureDeclarationKeyword(std::string_view txt) const noexcept { return structureKeywords.contains(txt); }
bool HoverHandler::IsStatementKeyword(std::string_view txt) const noexcept { return statementKeywords.contains(txt); }
bool HoverHandler::IsStorageModifierKeyword(std::string_view txt) const noexcept { return storageModifiers.contains(txt); }
bool HoverHandler::IsPrimitiveType(std::string_view txt) const noexcept { return primitiveTypes.contains(txt); }

bool HoverHandler::IsWord(std::string_view s) const noexcept
{
    if (s.empty())
        return false;

    unsigned char c = static_cast<unsigned char>(s[0]);
    return std::isalnum(c) || c == '_';
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

std::string_view HoverHandler::ExtractBaseTypeName(std::string_view typeStr) noexcept
{
    size_t pos = typeStr.find_first_of("@& <");

    if (pos != std::string_view::npos)
        return typeStr.substr(0, pos);

    return typeStr;
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
           tokenText == "?" || IsWord(tokenText);
}

void HoverHandler::NormalizeSignatureSpacing(std::string &signature) const
{
    size_t p = 0;

    while ((p = signature.find(" ::")) != std::string::npos)
    {
        signature.erase(p, 1);
    }
    while ((p = signature.find(":: ")) != std::string::npos)
    {
        signature.erase(p + 2, 1);
    }
    while ((p = signature.find("& in")) != std::string::npos)
    {
        signature.replace(p, 4, "&in");
    }
    while ((p = signature.find("& out")) != std::string::npos)
    {
        signature.replace(p, 5, "&out");
    }
    while ((p = signature.find("& inout")) != std::string::npos)
    {
        signature.replace(p, 7, "&inout");
    }
}

std::string HoverHandler::CleanSignature(std::string str)
{
    std::string res = "";
    res.reserve(str.size());

    for (size_t idx = 0; idx < str.size(); ++idx)
    {
        char c = str[idx];

        if (c == ' ')
        {
            if (!res.empty() && (res.back() == '<' || res.back() == '>' || res.back() == '@' || res.back() == '&' || res.back() == '('))
                continue;
            if (idx + 1 < str.size() && (str[idx + 1] == '<' || str[idx + 1] == '>' || str[idx + 1] == '@' || str[idx + 1] == '&' || str[idx + 1] == '(' || str[idx + 1] == ',' || str[idx + 1] == ')'))
                continue;
        }
        res += c;
    }

    std::string finalRes = "";
    finalRes.reserve(str.size());

    for (size_t idx = 0; idx < res.size(); ++idx)
    {
        finalRes += res[idx];

        if (res[idx] == '@' || res[idx] == '&' || res[idx] == ',')
        {
            if (idx + 1 < res.size() && res[idx + 1] != ' ' && res[idx + 1] != ',' && res[idx + 1] != ')' && res[idx + 1] != '>')
                finalRes += ' ';
        }
        if (res[idx] == '>')
        {
            if (idx + 1 < res.size() && res[idx + 1] != ' ' && res[idx + 1] != ',' && res[idx + 1] != ')' && res[idx + 1] != '>')
                finalRes += ' ';
        }
    }

    NormalizeSignatureSpacing(finalRes);
    return finalRes;
}

// =========================================================================
// STANDARD HOVER WORKFLOW ORCHESTRATION METHODS
// =========================================================================

HoverHandler::HoverHandler(asIScriptEngine *engine, std::string_view sourceCode, int line, int character)
    : nativeEng(engine), originalText(sourceCode), targetLine(line), targetCharacter(character)
{
}

bool HoverHandler::ParseType(size_t startIdx, size_t &nextIdx, std::string &typeStr)
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

void HoverHandler::TokenizePass()
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

int HoverHandler::FindTargetTokenIdx() noexcept
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

void HoverHandler::StructuralParsingPass()
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
                    ProcessNamespaceRule(k, fName, currentPrefix);
                else if (fType == "enum")
                    ProcessEnumRule(k, fName, currentPrefix);
                else
                    ProcessClassAndInterfaceRule(k, fName, fType, currentPrefix, static_cast<size_t>(look));
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
}

void HoverHandler::ProcessNamespaceRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix)
{
    declarations.push_back({fName, currentPrefix + fName, "namespace", "namespace " + currentPrefix + fName, allTokens[openBraceIdx].startPos, std::string::npos});
}

void HoverHandler::ProcessEnumRule(size_t openBraceIdx, const std::string &fName, const std::string &currentPrefix)
{
    declarations.push_back({fName, currentPrefix + fName, "enum", "enum " + currentPrefix + fName, allTokens[openBraceIdx].startPos, std::string::npos});
    size_t enumScan = openBraceIdx + 1;
    int enumVal = 0;

    while (enumScan < allTokens.size() && allTokens[enumScan].text != "}")
    {
        if (allTokens[enumScan].tokenClass == asTC_IDENTIFIER && IsWord(allTokens[enumScan].text))
        {
            std::string eNameStr = std::string(allTokens[enumScan].text);
            declarations.push_back({eNameStr, currentPrefix + fName + "::" + eNameStr, "enum_value", "enum " + currentPrefix + fName + "::" + eNameStr + " = " + std::to_string(enumVal), allTokens[enumScan].startPos, allTokens[enumScan].endPos});
            enumVal++;
        }
        enumScan++;
    }
}

void HoverHandler::ProcessClassAndInterfaceRule(size_t openBraceIdx, const std::string &fName, const std::string &fType, const std::string &currentPrefix, size_t lookaheadIdx)
{
    declarations.push_back({fName, currentPrefix + fName, fType, fType + " " + currentPrefix + fName, allTokens[openBraceIdx].startPos, std::string::npos});

    if (fType == "class" || fType == "interface" || fType == "mixin class" || fType == "abstract class")
    {
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
}

void HoverHandler::ProcessFuncDefRule(size_t &idxScan, const std::string &currentPrefix)
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
        ProcessForLoopRule(idxScan);
    else if (allTokens[idxScan].text == "foreach")
        ProcessForeachLoopRule(idxScan);
    else
        idxScan++;
}

void HoverHandler::ProcessForLoopRule(size_t &idxScan)
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

                if (IsAutoDeducibleType(loopTypeStr) && nameTokenIdx + 1 < allTokens.size() && allTokens[nameTokenIdx + 1].text == "=")
                {
                    std::string deduced = DeduceTypeFromRHS(nameTokenIdx + 2);

                    if (deduced != "auto")
                        loopTypeStr = deduced;
                }

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
                    declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[nameTokenIdx].startPos, activeFuncEnd});
                }
            }
        }
    }

    idxScan++;
}

void HoverHandler::ProcessForeachLoopRule(size_t &idxScan)
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
                std::string containerName = "";

                if (nextIdx + 1 < allTokens.size() && allTokens[nextIdx + 1].text == "in")
                {
                    if (nextIdx + 2 < allTokens.size() && IsWord(allTokens[nextIdx + 2].text))
                    {
                        containerName = std::string(allTokens[nextIdx + 2].text);
                    }
                }

                if (IsAutoDeducibleType(loopTypeStr) && !containerName.empty())
                {
                    for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                    {
                        if (it->name == containerName)
                        {
                            size_t lastSpace = it->hoverText.rfind(' ');

                            if (lastSpace != std::string::npos)
                            {
                                std::string containerType = it->hoverText.substr(0, lastSpace);
                                size_t startAngle = containerType.find('<');
                                size_t endAngle = containerType.rfind('>');

                                if (startAngle != std::string::npos && endAngle != std::string::npos && endAngle > startAngle)
                                {
                                    loopTypeStr = containerType.substr(startAngle + 1, endAngle - startAngle - 1);
                                }
                            }
                            break;
                        }
                    }
                }

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
                    declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[nameTokenIdx].startPos, activeFuncEnd});
                }
            }
        }
    }

    idxScan++;
}

void HoverHandler::ProcessScriptRule()
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
            std::string tdName(allTokens[idxScan + 2].text);
            declarations.push_back({tdName, tdName, "typedef", "typedef " + std::string(allTokens[idxScan + 1].text) + " " + tdName, allTokens[idxScan + 2].startPos, allTokens[idxScan + 2].endPos});
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

                    bool isFunctionDef = (modScan < allTokens.size() && allTokens[modScan].text == "{") ||
                                         (activeClassStart != std::string::npos && activeFuncStart == std::string::npos) ||
                                         (activeClassStart == std::string::npos && activeFuncStart == std::string::npos && modifiers.find("import") != std::string::npos);

                    if (isFunctionDef)
                    {
                        std::vector<HoverUtils::FuncParam> funcParams;
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
                                    funcParams.push_back(HoverUtils::FuncParam{std::string(nTok.text), ptStr});
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
                        std::string finalTypeStr = typeStr;

                        if (IsAutoDeducibleType(typeStr) && allTokens[nameIdx + 1].text == "=" && nameIdx + 2 < allTokens.size())
                        {
                            std::string deduced = DeduceTypeFromRHS(nameIdx + 2);

                            if (deduced != "auto")
                            {
                                finalTypeStr = deduced;

                                if (typeStr.find('@') != std::string::npos && finalTypeStr.find('@') == std::string::npos)
                                    finalTypeStr += "@";
                            }
                        }

                        if (activeFuncStart != std::string::npos)
                            declarations.push_back({entityName, entityName, "local_variable", finalTypeStr + " " + entityName, activeFuncStart, activeFuncEnd});
                        else if (activeClassStart != std::string::npos)
                            declarations.push_back({entityName, currentPrefix + entityName, "property", finalTypeStr + " " + currentPrefix + entityName, activeClassStart, activeClassEnd});
                        else
                            declarations.push_back({entityName, currentPrefix + entityName, "global_variable", finalTypeStr + " " + currentPrefix + entityName, allTokens[nameIdx].startPos, std::string::npos});

                        size_t searchComma = HoverUtils::ScanInlineChainedDeclarations(allTokens, closeParen + 1, allTokens.size());

                        if (searchComma < allTokens.size() && allTokens[searchComma].text == ",")
                        {
                            innerScan = searchComma + 1;
                            continue;
                        }

                        idxScan = (searchComma < allTokens.size()) ? searchComma + 1 : searchComma;
                        statementMatched = true;
                        break;
                    }
                }
                else if (nameIdx + 1 < allTokens.size() && allTokens[nameIdx + 1].text == "{")
                {
                    std::string modifiers = " property";
                    size_t innerProp = nameIdx + 2;

                    while (innerProp < allTokens.size() && allTokens[innerProp].text != "}")
                    {
                        if (allTokens[innerProp].text == "const" || allTokens[innerProp].text == "override")
                        {
                            modifiers += " ";
                            modifiers += allTokens[innerProp].text;
                        }
                        innerProp++;
                    }

                    declarations.push_back({entityName, currentPrefix + entityName, "property", typeStr + " " + currentPrefix + entityName + modifiers, allTokens[nameIdx].startPos, allTokens[innerProp].endPos});
                    idxScan = innerProp + 1;
                    statementMatched = true;
                    break;
                }
                else if (nameIdx + 1 < allTokens.size() && (allTokens[nameIdx + 1].text == ";" || allTokens[nameIdx + 1].text == "=" || allTokens[nameIdx + 1].text == "," || allTokens[nameIdx + 1].text == "[" || allTokens[nameIdx + 1].text == ")"))
                {
                    std::string finalTypeStr = typeStr;

                    if (IsAutoDeducibleType(typeStr) && allTokens[nameIdx + 1].text == "=" && nameIdx + 2 < allTokens.size())
                    {
                        std::string deduced = DeduceTypeFromRHS(nameIdx + 2);

                        if (deduced != "auto")
                        {
                            finalTypeStr = deduced;

                            if (typeStr.find('@') != std::string::npos && finalTypeStr.find('@') == std::string::npos)
                                finalTypeStr += "@";
                        }
                    }

                    if (activeFuncStart != std::string::npos)
                        declarations.push_back({entityName, entityName, "local_variable", finalTypeStr + " " + entityName, activeFuncStart, activeFuncEnd});
                    else if (activeClassStart != std::string::npos)
                        declarations.push_back({entityName, currentPrefix + entityName, "property", finalTypeStr + " " + currentPrefix + entityName, activeClassStart, activeClassEnd});
                    else
                        declarations.push_back({entityName, currentPrefix + entityName, "global_variable", finalTypeStr + " " + currentPrefix + entityName, allTokens[nameIdx].startPos, std::string::npos});

                    size_t searchComma = HoverUtils::ScanInlineChainedDeclarations(allTokens, nameIdx + 1, allTokens.size());

                    if (searchComma < allTokens.size() && allTokens[searchComma].text == ",")
                    {
                        innerScan = searchComma + 1;
                        continue;
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

void HoverHandler::ProcessLambdaRule(size_t &k)
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
        std::vector<HoverUtils::FuncParam> funcParams;

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
                    funcParams.push_back(HoverUtils::FuncParam{std::string(nTok.text), ptStr});
                }
                pToks.clear();
            }
            else
            {
                pToks.push_back(allTokens[idxP]);
            }
        }

        std::string inferredRet = "auto";
        int lookBack = static_cast<int>(nameIdx) - 1;

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
            std::string targetVar = std::string(allTokens[lookBack].text);

            for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                if (it->name == targetVar && (it->type == "local_variable" || it->type == "property" || it->type == "global_variable" || it->type == "parameter"))
                {
                    size_t spacePos = it->hoverText.rfind(' ');

                    if (spacePos != std::string::npos)
                    {
                        std::string_view accessStrippedView = StripAccessModifiers(std::string_view(it->hoverText).substr(0, spacePos));
                        inferredRet = std::string(accessStrippedView);

                        while (!inferredRet.empty() && (inferredRet.back() == '@' || inferredRet.back() == '&'))
                        {
                            inferredRet.pop_back();
                        }
                    }
                    break;
                }
            }
        }

        size_t modScan = closeParen + 1;
        size_t bodyBraceIdx = std::string::npos;

        if (modScan < allTokens.size() && allTokens[modScan].text == "{")
            bodyBraceIdx = modScan;

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
                paramsStr += ", ";

            paramsStr += funcParams[p].pType + " " + funcParams[p].pName;
        }

        std::string hoverTxt = inferredRet + " function(" + paramsStr + ")";
        declarations.push_back({"function", "function", "lambda", hoverTxt, allTokens[nameIdx].startPos, functionEndPos});

        if (functionEndPos != std::string::npos)
        {
            for (const auto &fp : funcParams)
                declarations.push_back({fp.pName, fp.pName, "parameter", fp.pType + " " + fp.pName, allTokens[nameIdx].startPos, functionEndPos});
        }

        if (bodyBraceIdx != std::string::npos)
        {
            size_t lambdaBraceDepth = 1;
            size_t scanIdx = bodyBraceIdx + 1;
            size_t endBraceTokenIdx = bodyBraceIdx;

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

            size_t lambdaIdxScan = bodyBraceIdx + 1;

            while (lambdaIdxScan < endBraceTokenIdx)
            {
                std::string_view lTxt = allTokens[lambdaIdxScan].text;

                if (lTxt == "foreach")
                {
                    size_t loopIdx = lambdaIdxScan + 1;

                    if (loopIdx < endBraceTokenIdx && allTokens[loopIdx].text == "(")
                    {
                        size_t nextIdxLoop = loopIdx + 1;
                        std::string loopTypeStr = "";

                        if (ParseType(loopIdx + 1, nextIdxLoop, loopTypeStr))
                        {
                            if (nextIdxLoop < endBraceTokenIdx && IsWord(allTokens[nextIdxLoop].text) && !reservedKeywords.contains(allTokens[nextIdxLoop].text))
                            {
                                std::string loopVarName = std::string(allTokens[nextIdxLoop].text);
                                std::string containerName = "";

                                if (nextIdxLoop + 1 < endBraceTokenIdx && allTokens[nextIdxLoop + 1].text == "in" && nextIdxLoop + 2 < endBraceTokenIdx && IsWord(allTokens[nextIdxLoop + 2].text))
                                {
                                    containerName = std::string(allTokens[nextIdxLoop + 2].text);
                                }

                                if (IsAutoDeducibleType(loopTypeStr) && !containerName.empty())
                                {
                                    for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                                    {
                                        if (it->name == containerName)
                                        {
                                            size_t lastSpaceLoop = it->hoverText.rfind(' ');

                                            if (lastSpaceLoop != std::string::npos)
                                            {
                                                std::string containerType = it->hoverText.substr(0, lastSpaceLoop);
                                                size_t startAngle = containerType.find('<');
                                                size_t endAngle = containerType.rfind('>');

                                                if (startAngle != std::string::npos && endAngle != std::string::npos && endAngle > startAngle)
                                                    loopTypeStr = containerType.substr(startAngle + 1, endAngle - startAngle - 1);
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
                    size_t loopIdx = lambdaIdxScan + 1;

                    if (loopIdx < endBraceTokenIdx && allTokens[loopIdx].text == "(")
                    {
                        size_t nextIdxLoop = loopIdx + 1;
                        std::string loopTypeStr = "";

                        if (ParseType(loopIdx + 1, nextIdxLoop, loopTypeStr))
                        {
                            if (nextIdxLoop < endBraceTokenIdx && IsWord(allTokens[nextIdxLoop].text) && !reservedKeywords.contains(allTokens[nextIdxLoop].text))
                            {
                                std::string loopVarName = std::string(allTokens[nextIdxLoop].text);
                                size_t nameTokenIdx = nextIdxLoop;

                                if (IsAutoDeducibleType(loopTypeStr) && nameTokenIdx + 1 < endBraceTokenIdx && allTokens[nameTokenIdx + 1].text == "=")
                                {
                                    std::string deducedFor = DeduceTypeFromRHS(nameTokenIdx + 2);

                                    if (deducedFor != "auto")
                                        loopTypeStr = deducedFor;
                                }
                                declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[nameTokenIdx].startPos, functionEndPos});
                            }
                        }
                    }
                    lambdaIdxScan++;
                    continue;
                }

                std::string typeStr = "";
                size_t nextIdx = lambdaIdxScan;

                if (ParseType(lambdaIdxScan, nextIdx, typeStr))
                {
                    if (nextIdx <= lambdaIdxScan)
                    {
                        lambdaIdxScan++;
                        continue;
                    }

                    std::string_view baseType = allTokens[lambdaIdxScan].text;

                    if (IsStatementKeyword(baseType))
                    {
                        lambdaIdxScan = nextIdx;
                        continue;
                    }

                    size_t innerScan = nextIdx;
                    bool advanced = false;

                    while (innerScan < endBraceTokenIdx && IsWord(allTokens[innerScan].text) && !reservedKeywords.contains(allTokens[innerScan].text))
                    {
                        size_t nameIdxLocal = innerScan;
                        std::string entityName = std::string(allTokens[nameIdxLocal].text);

                        if (nameIdxLocal + 1 < allTokens.size() && (allTokens[nameIdxLocal + 1].text == ";" || allTokens[nameIdxLocal + 1].text == "=" || allTokens[nameIdxLocal + 1].text == "," || allTokens[nameIdxLocal + 1].text == "[" || allTokens[nameIdxLocal + 1].text == ")"))
                        {
                            std::string finalTypeStr = typeStr;

                            if (IsAutoDeducibleType(typeStr) && allTokens[nameIdxLocal + 1].text == "=" && nameIdxLocal + 2 < allTokens.size())
                            {
                                std::string deducedLocal = DeduceTypeFromRHS(nameIdxLocal + 2);

                                if (deducedLocal != "auto")
                                {
                                    finalTypeStr = deducedLocal;

                                    if (typeStr.find('@') != std::string::npos && finalTypeStr.find('@') == std::string::npos)
                                        finalTypeStr += "@";
                                }
                            }

                            declarations.push_back({entityName, entityName, "local_variable", finalTypeStr + " " + entityName, allTokens[bodyBraceIdx].startPos, functionEndPos});
                            size_t searchComma = HoverUtils::ScanInlineChainedDeclarations(allTokens, nameIdxLocal + 1, endBraceTokenIdx);

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
    if (startIdx >= allTokens.size())
        return "auto";

    size_t curIdx = startIdx;

    if (allTokens[curIdx].text == "@")
        curIdx++;

    if (curIdx >= allTokens.size())
        return "auto";

    if (allTokens[curIdx].tokenClass == asTC_VALUE)
    {
        if (!allTokens[curIdx].text.empty() && (allTokens[curIdx].text[0] == '"' || allTokens[curIdx].text[0] == '\'' || allTokens[curIdx].text.starts_with("\"\"\"")))
        {
            return "string";
        }
        if (!allTokens[curIdx].text.empty() && std::isdigit(static_cast<unsigned char>(allTokens[curIdx].text[0])))
        {
            if (allTokens[curIdx].text.find('.') != std::string_view::npos || allTokens[curIdx].text.back() == 'f')
                return "float";

            return "int";
        }
    }

    std::string_view firstToken = allTokens[curIdx].text;

    if (IsPrimitiveType(firstToken))
        return std::string(firstToken);

    for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
    {
        if (it->name == firstToken || (it->fullName.length() >= firstToken.length() && it->fullName.substr(it->fullName.length() - firstToken.length()) == firstToken))
        {
            size_t lastSpace = it->hoverText.rfind(' ');

            if (lastSpace != std::string::npos)
            {
                std::string deduced = it->hoverText.substr(0, lastSpace);

                if (curIdx + 1 < allTokens.size() && allTokens[curIdx + 1].text == "[")
                {
                    size_t startAngle = deduced.find('<');
                    size_t endAngle = deduced.rfind('>');

                    if (startAngle != std::string::npos && endAngle != std::string::npos && endAngle > startAngle)
                        deduced = deduced.substr(startAngle + 1, endAngle - startAngle - 1);
                }
                return deduced;
            }
        }
    }

    if (nativeEng && nativeEng->GetTypeInfoByName(std::string(firstToken).c_str()))
        return std::string(firstToken);

    return "auto";
}

std::string HoverHandler::EnhanceIfFuncdef(const std::string &hoverText)
{
    std::string baseHover = hoverText;

    if (!nativeEng)
        return baseHover;

    std::string_view cleanedView = StripAccessModifiers(baseHover);
    size_t firstSpace = cleanedView.find(' ');

    if (firstSpace != std::string_view::npos)
    {
        std::string typeName = std::string(cleanedView.substr(0, firstSpace));

        if (!typeName.empty() && typeName.back() == '@')
            typeName.pop_back();

        std::string paramsSuffix = "";

        for (auto dIt = declarations.begin(); dIt != declarations.end(); ++dIt)
        {
            if (dIt->type == "funcdef" && dIt->name == typeName)
            {
                size_t paren = dIt->hoverText.find('(');

                if (paren != std::string::npos)
                {
                    paramsSuffix = dIt->hoverText.substr(paren);
                    break;
                }
            }
        }

        if (paramsSuffix.empty())
        {
            asIScriptModule *mod = nativeEng->GetModule("LSPModule");
            asITypeInfo *fdefInfo = mod ? mod->GetTypeInfoByName(typeName.c_str()) : nullptr;

            if (!fdefInfo)
                fdefInfo = nativeEng->GetTypeInfoByName(typeName.c_str());

            if (fdefInfo && (fdefInfo->GetFlags() & asOBJ_FUNCDEF))
            {
                asIScriptFunction *fdefFunc = fdefInfo->GetFuncdefSignature();

                if (fdefFunc)
                {
                    std::string fullFuncDecl = fdefFunc->GetDeclaration(true, true);
                    size_t parenPos = fullFuncDecl.find('(');

                    if (parenPos != std::string::npos)
                        paramsSuffix = fullFuncDecl.substr(parenPos);
                }
            }
        }

        if (!paramsSuffix.empty())
            baseHover += paramsSuffix;
    }

    return baseHover;
}

bool HoverHandler::MatchesQual(const DeclInfo &decl, const std::string &fullQualName) const noexcept
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

std::string HoverHandler::SemanticValidationPass(int targetIdx)
{
    if (targetIdx == -1)
        return "";

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
                if ((it->type != "local_variable" && it->type != "parameter") || (targetTok.startPos >= it->startPos && targetTok.startPos <= it->endPos))
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
                        return fmt::format("```cpp\n{}\n```", CleanSignature(EnhanceIfFuncdef(it->hoverText)));
                    }
                }

                if (nativeEng)
                {
                    asITypeInfo *typeInfo = nativeEng->GetTypeInfoByName(typeHierarchyStr.c_str());

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
                                    declStr.erase(tppos, 5);

                                if (declStr.find(std::string(cleanTypeName) + "::") == std::string::npos)
                                {
                                    size_t namePos = declStr.find(std::string(targetTok.text) + "(");

                                    if (namePos != std::string::npos)
                                        declStr.insert(namePos, objType + "::");
                                }

                                size_t openBracket = objType.find('<');
                                size_t closeBracket = objType.rfind('>');

                                if (openBracket != std::string::npos && closeBracket != std::string::npos && closeBracket > openBracket)
                                {
                                    std::string templateArg = objType.substr(openBracket + 1, closeBracket - openBracket - 1);
                                    size_t tPos = 0;

                                    while ((tPos = declStr.find('T', tPos)) != std::string::npos)
                                    {
                                        bool leftOk = (tPos == 0 || (!std::isalnum(static_cast<unsigned char>(declStr[tPos - 1])) && declStr[tPos - 1] != '_'));
                                        bool rightOk = (tPos + 1 == declStr.length() || (!std::isalnum(static_cast<unsigned char>(declStr[tPos + 1])) && declStr[tPos + 1] != '_'));

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
                                return fmt::format("```cpp\n{}\n```", CleanSignature(declStr));
                            }
                        }
                    }
                }
            }
        }
        return "";
    }

    if (targetTok.tokenClass == asTC_VALUE)
    {
        if (!targetTok.text.empty() && (targetTok.text[0] == '"' || targetTok.text[0] == '\'' || targetTok.text.starts_with("\"\"\"")))
        {
            int chainStart = targetIdx;
            int chainEnd = targetIdx;
            std::string combinedLiteral = "\"";
            size_t totalInternalLength = 0;

            // Scan backward for contiguous string segments
            while (chainStart > 0)
            {
                std::string_view prevText = allTokens[chainStart - 1].text;

                if (allTokens[chainStart - 1].tokenClass == asTC_VALUE &&
                    !prevText.empty() &&
                    (prevText[0] == '"' || prevText[0] == '\'' || prevText.starts_with("\"\"\"")))
                {
                    chainStart--;
                }
                else
                {
                    break;
                }
            }

            // Scan forward for contiguous string segments
            while (chainEnd + 1 < static_cast<int>(allTokens.size()))
            {
                std::string_view nextText = allTokens[chainEnd + 1].text;

                if (allTokens[chainEnd + 1].tokenClass == asTC_VALUE &&
                    !nextText.empty() &&
                    (nextText[0] == '"' || nextText[0] == '\'' || nextText.starts_with("\"\"\"")))
                {
                    chainEnd++;
                }
                else
                {
                    break;
                }
            }

            for (int cIdx = chainStart; cIdx <= chainEnd; ++cIdx)
            {
                std::string_view tText = allTokens[cIdx].text;

                if (tText.starts_with("\"\"\"") && tText.length() >= 6)
                {
                    combinedLiteral += tText.substr(3, tText.length() - 6);
                    totalInternalLength += (tText.length() - 6);
                }
                else if (tText.length() >= 2)
                {
                    combinedLiteral += tText.substr(1, tText.length() - 2);
                    totalInternalLength += (tText.length() - 2);
                }
            }

            combinedLiteral += "\"";
            size_t finalArraySize = totalInternalLength + 1;

            return fmt::format("```cpp\n(const char [{}]) {}\n```", finalArraySize, combinedLiteral);
        }

        if (targetTok.text.find('.') != std::string_view::npos || targetTok.text.back() == 'f')
        {
            return fmt::format("```cpp\n(float) {}\n```", targetTok.text);
        }

        return fmt::format("```cpp\n(int) {}\n```", targetTok.text);
    }

    if ((reservedKeywords.contains(targetTok.text) || contextualKeywords.contains(targetTok.text)) && !primitiveTypes.contains(targetTok.text))
    {
        if (!IsKeywordHoverException(targetTok.text))
            return "";
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

        for (auto itDecls = declarations.rbegin(); itDecls != declarations.rend(); ++itDecls)
        {
            if ((itDecls->type == "local_variable" || itDecls->type == "parameter" || itDecls->type == "global_variable") && itDecls->name == targetTok.text)
            {
                if (itDecls->type == "global_variable" || (targetTok.startPos >= itDecls->startPos && targetTok.startPos <= itDecls->endPos))
                {
                    return fmt::format("```cpp\n{}\n```", CleanSignature(EnhanceIfFuncdef(itDecls->hoverText)));
                }
            }
        }

        if (isFollowedByParen || isPrecededByTilde || targetTok.text == "function")
        {
            for (auto itDecls = declarations.rbegin(); itDecls != declarations.rend(); ++itDecls)
            {
                if ((itDecls->type == "function" || itDecls->type == "lambda") && MatchesQual(*itDecls, fullQualName))
                {
                    if (itDecls->type == "lambda" && targetTok.startPos != itDecls->startPos)
                        continue;

                    return fmt::format("```cpp\n{}\n```", CleanSignature(itDecls->hoverText));
                }
            }
        }

        for (auto itDecls = declarations.rbegin(); itDecls != declarations.rend(); ++itDecls)
        {
            if (itDecls->type != "function" && itDecls->type != "local_variable" && itDecls->type != "parameter" && itDecls->type != "global_variable" && itDecls->type != "lambda" && MatchesQual(*itDecls, fullQualName))
            {
                return fmt::format("```cpp\n{}\n```", CleanSignature(EnhanceIfFuncdef(itDecls->hoverText)));
            }
        }

        if (nativeEng)
        {
            for (asUINT f = 0; f < nativeEng->GetFuncdefCount(); ++f)
            {
                asITypeInfo *fInfo = nativeEng->GetFuncdefByIndex(f);

                if (fInfo && fInfo->GetName() == targetTok.text)
                {
                    asIScriptFunction *fFunc = fInfo->GetFuncdefSignature();

                    if (fFunc)
                        return fmt::format("```cpp\nfuncdef {}\n```", CleanSignature(fFunc->GetDeclaration(true, true)));
                }
            }

            asITypeInfo *tInfo = nativeEng->GetTypeInfoByName(std::string(targetTok.text).c_str());

            if (tInfo)
                return fmt::format("```cpp\nclass {}\n```", tInfo->GetName());
        }
    }

    return "";
}

json HoverHandler::Process()
{
    TokenizePass();
    int targetIdx = FindTargetTokenIdx();

    StructuralParsingPass();
    ProcessScriptRule();

    for (size_t k = 0; k < allTokens.size(); ++k)
    {
        if (allTokens[k].text == "function")
        {
            ProcessLambdaRule(k);
        }
    }

    std::string resultMarkdown = SemanticValidationPass(targetIdx);

    if (!resultMarkdown.empty())
    {
        return {{"contents", {{"kind", "markdown"}, {"value", resultMarkdown}}}};
    }

    return nullptr;
}