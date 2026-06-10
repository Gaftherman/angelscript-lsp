/**
 * @file HoverHandler.cpp
 * @brief Implementation of the HoverHandler class for language server hover functionality.
 */

#include "HoverHandler.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fmt/core.h>

// Initialization of static constants
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

/**
 * @class HoverHandler
 * @brief Processes source code tokens to provide hover information in an AngelScript/C++ environment.
 */

/**
 * @brief Constructs a HoverHandler instance.
 * @param engine Pointer to the native AngelScript engine instance.
 * @param sourceCode The complete source code being analyzed.
 * @param line The target line number (0-indexed) for the hover query.
 * @param character The target character position (0-indexed) for the hover query.
 */
HoverHandler::HoverHandler(asIScriptEngine *engine, const std::string &sourceCode, int line, int character)
    : nativeEng(engine), originalText(sourceCode), targetLine(line), targetCharacter(character)
{
}

/**
 * @brief Checks if a string represents a valid word or identifier token.
 * @param s The string to validate.
 * @return True if the string is a valid word token, false otherwise.
 */
bool HoverHandler::IsWord(const std::string &s)
{
    if (s.empty())
        return false;
    char c = s[0];
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/**
 * @brief Cleans and normalizes white spaces in a function signature for standard formatting.
 * @param str The raw signature string.
 * @return The cleaned and formatted signature string.
 */
std::string HoverHandler::CleanSignature(std::string str)
{
    std::string res = "";
    std::string finalRes = "";
    size_t p = 0;

    // 1. Initial space compression around common special characters
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

    // 2. Strict control of output spacing for references and templates
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
            if (idx + 1 < res.size() && res[idx + 1] != ' ' && res[idx + 1] != ',' && res[idx + 1] != ')' && res[idx + 1] != '>' && res[idx + 1] != '@' && res[idx + 1] != '&' && res[idx + 1] != ':')
                finalRes += ' ';
        }
    }

    // 3. Remove spaces before and after the scope resolution operator "::"
    while ((p = finalRes.find(" ::")) != std::string::npos)
        finalRes.erase(p, 1);
    while ((p = finalRes.find(":: ")) != std::string::npos)
        finalRes.erase(p + 2, 1);

    // 4. Normalization of compound native references
    while ((p = finalRes.find("& in")) != std::string::npos)
        finalRes.replace(p, 4, "&in");
    while ((p = finalRes.find("& out")) != std::string::npos)
        finalRes.replace(p, 5, "&out");
    while ((p = finalRes.find("& inout")) != std::string::npos)
        finalRes.replace(p, 7, "&inout");

    return finalRes;
}

/**
 * @brief Parses a type identifier starting at a specific token index.
 * @param startIdx The token index to start parsing from.
 * @param nextIdx Output parameter that receives the index of the token following the parsed type.
 * @param typeStr Output string where the parsed type representation is accumulated.
 * @return True if a valid type was successfully parsed, false otherwise.
 */
bool HoverHandler::ParseType(size_t startIdx, size_t &nextIdx, std::string &typeStr)
{
    size_t idx = startIdx;
    std::vector<size_t> typeTokenIndices;
    int continuousWords = 0;

    if (idx >= allTokens.size())
        return false;

    while (idx < allTokens.size() && (storageModifiers.count(allTokens[idx].text) || allTokens[idx].text == "shared" || allTokens[idx].text == "external"))
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
        std::string txt = allTokens[idx].text;
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

    for (int k = (int)typeTokenIndices.size() - 1; k >= 0; --k)
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
                depth++;
            else if (txt == ">")
                depth--;
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

/**
 * @brief Performs the initial tokenization pass on the raw source code text.
 */
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
                    curChar++;
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
                curChar++;
        }
        pos += len;
    }
}

/**
 * @brief Finds the index of the token corresponding to the target line and character coordinates.
 * @return The token index if found, or -1 if no matching token exists at the target coordinates.
 */
int HoverHandler::FindTargetTokenIdx()
{
    for (int k = 0; k < (int)allTokens.size(); ++k)
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
                int currentLineLength = (int)(i - segmentStart);

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

/**
 * @brief Analyzes the scope structure and tracks blocks, namespaces, classes, and functions.
 */
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
            int look = (int)k - 1;
            std::string fType = "other";
            std::string fName = "";
            bool hasParen = false;

            while (look >= 0)
            {
                std::string t = allTokens[look].text;
                if (t == "}" || t == ";")
                    break;
                if (t == ")")
                    hasParen = true;

                if (t == "namespace" || t == "class" || t == "interface" || t == "enum")
                {
                    fType = t;
                    if (look + 1 < (int)allTokens.size())
                        fName = allTokens[look + 1].text;
                    if (t == "class" && look > 0 && allTokens[look - 1].text == "mixin")
                        fType = "mixin class";
                    if (t == "class" && look > 0 && allTokens[look - 1].text == "abstract")
                        fType = "abstract class";

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
                    // Section to capture anonymous functions (lambdas)
                    else if (possibleFuncName == "function")
                    {
                        fType = "function";
                        fName = "lambda"; // Internal identifier for the closure scope
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
}

/**
 * @brief Scans the token stream to extract object, variable, loop, and function declarations.
 */
void HoverHandler::ExtractDeclarationsPass()
{
    size_t idxScan = 0;

    // Expression deduction sub-analyzer for "auto" inference
    auto DeduceTypeFromRHS = [&](size_t startIdx) -> std::string
    {
        if (startIdx >= allTokens.size())
            return "auto";

        size_t curIdx = startIdx;
        if (allTokens[curIdx].text == "@")
        {
            curIdx++;
        }

        if (curIdx >= allTokens.size())
            return "auto";

        // A. Detect string literals
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

        std::string firstToken = allTokens[curIdx].text;

        // B. Detect primitive types / explicit casts (e.g., uint(0), int(...))
        if (primitiveTypes.count(firstToken))
        {
            return firstToken;
        }

        // C. Look for matching entries in previous declarations (variables/parameters)
        for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
        {
            if (it->name == firstToken || (it->fullName.length() >= firstToken.length() &&
                                           it->fullName.substr(it->fullName.length() - firstToken.length()) == firstToken))
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
                        {
                            deduced = deduced.substr(startAngle + 1, endAngle - startAngle - 1);
                        }
                    }
                    return deduced;
                }
            }
        }

        // D. Verify native engine types
        if (nativeEng)
        {
            if (nativeEng->GetTypeInfoByName(firstToken.c_str()))
            {
                return firstToken;
            }
        }
        return "auto";
    };

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

        // Automatic type inference in foreach loops
        if (txt == "foreach")
        {
            size_t loopIdx = idxScan + 1;
            if (loopIdx < allTokens.size() && allTokens[loopIdx].text == "(")
            {
                size_t nextIdx = loopIdx + 1;
                std::string loopTypeStr = "";
                if (ParseType(loopIdx + 1, nextIdx, loopTypeStr))
                {
                    if (nextIdx < allTokens.size() && IsWord(allTokens[nextIdx].text) && !reservedKeywords.count(allTokens[nextIdx].text))
                    {
                        std::string loopVarName = allTokens[nextIdx].text;
                        size_t nameTokenIdx = nextIdx;

                        std::string containerName = "";
                        if (nextIdx + 1 < allTokens.size() && allTokens[nextIdx + 1].text == "in")
                        {
                            if (nextIdx + 2 < allTokens.size() && IsWord(allTokens[nextIdx + 2].text))
                            {
                                containerName = allTokens[nextIdx + 2].text;
                            }
                        }

                        if (loopTypeStr.find("auto") != std::string::npos && !containerName.empty())
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
            continue;
        }

        // Sequential extraction of traditional for loop variables (inferred)
        if (txt == "for")
        {
            size_t loopIdx = idxScan + 1;
            if (loopIdx < allTokens.size() && allTokens[loopIdx].text == "(")
            {
                size_t nextIdx = loopIdx + 1;
                std::string loopTypeStr = "";
                if (ParseType(loopIdx + 1, nextIdx, loopTypeStr))
                {
                    if (nextIdx < allTokens.size() && IsWord(allTokens[nextIdx].text) && !reservedKeywords.count(allTokens[nextIdx].text))
                    {
                        std::string loopVarName = allTokens[nextIdx].text;
                        size_t nameTokenIdx = nextIdx;

                        if (loopTypeStr.find("auto") != std::string::npos)
                        {
                            if (nameTokenIdx + 1 < allTokens.size() && allTokens[nameTokenIdx + 1].text == "=")
                            {
                                std::string deduced = DeduceTypeFromRHS(nameTokenIdx + 2);
                                if (deduced != "auto")
                                {
                                    loopTypeStr = deduced;
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
            continue;
        }

        if (txt == "funcdef")
        {
            size_t nextIdx = idxScan + 1;
            std::string typeStr = "";
            if (ParseType(idxScan + 1, nextIdx, typeStr))
            {
                if (nextIdx < allTokens.size() && allTokens[nextIdx].text == "@")
                    nextIdx++;
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

            if (baseType == "return" || baseType == "if" || baseType == "while" ||
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
                            modifiers += " " + allTokens[modScan].text;
                        modScan++;
                    }

                    bool isFunctionDef = (modScan < allTokens.size() && allTokens[modScan].text == "{") ||
                                         (activeClassStart != std::string::npos && activeFuncStart == std::string::npos) ||
                                         (activeClassStart == std::string::npos && activeFuncStart == std::string::npos && modifiers.find("import") != std::string::npos);

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
                                    pToks.push_back(allTokens[k]);
                                if (!pToks.empty() && IsWord(pToks.back().text) && !reservedKeywords.count(pToks.back().text))
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
                                    funcParams.push_back({nTok.text, ptStr});
                                }
                                pToks.clear();
                            }
                            else
                                pToks.push_back(allTokens[k]);
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
                                paramsStr += ", ";
                            paramsStr += funcParams[p].pType + " " + funcParams[p].pName;
                        }

                        std::string fullQualFunc = currentPrefix + entityName;
                        declarations.push_back({entityName, fullQualFunc, "function", typeStr + " " + fullQualFunc + "(" + paramsStr + ")" + modifiers, allTokens[nameIdx].startPos, functionEndPos});

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
                        if (typeStr.find("auto") != std::string::npos)
                        {
                            if (allTokens[nameIdx + 1].text == "=" && nameIdx + 2 < allTokens.size())
                            {
                                std::string deduced = DeduceTypeFromRHS(nameIdx + 2);
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
                            modifiers += " " + allTokens[inner].text;
                        inner++;
                    }

                    declarations.push_back({entityName, currentPrefix + entityName, "property", typeStr + " " + currentPrefix + entityName + modifiers, allTokens[nameIdx].startPos, allTokens[inner].endPos});
                    idxScan = inner + 1;
                    statementMatched = true;
                    break;
                }
                else if (nameIdx + 1 < allTokens.size() && (allTokens[nameIdx + 1].text == ";" || allTokens[nameIdx + 1].text == "=" || allTokens[nameIdx + 1].text == "," || allTokens[nameIdx + 1].text == "[" || allTokens[nameIdx + 1].text == ")"))
                {
                    std::string finalTypeStr = typeStr;
                    if (typeStr.find("auto") != std::string::npos)
                    {
                        if (allTokens[nameIdx + 1].text == "=" && nameIdx + 2 < allTokens.size())
                        {
                            std::string deduced = DeduceTypeFromRHS(nameIdx + 2);
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
                continue;
            idxScan = nextIdx;
            continue;
        }
        idxScan++;
    }
}

/**
 * @brief Scans the token stream specifically to extract and process anonymous lambda functions and closures.
 */
void HoverHandler::ExtractLambdasPass()
{
    // Expression deduction sub-analyzer for internal "auto" inference
    auto DeduceTypeFromRHS = [&](size_t startIdx) -> std::string
    {
        if (startIdx >= allTokens.size())
            return "auto";

        size_t curIdx = startIdx;
        if (allTokens[curIdx].text == "@")
        {
            curIdx++;
        }

        if (curIdx >= allTokens.size())
            return "auto";

        // A. Detect string literals
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

        std::string firstToken = allTokens[curIdx].text;

        // B. Detect primitive types / explicit casts (e.g., uint(0), int(...))
        if (primitiveTypes.count(firstToken))
        {
            return firstToken;
        }

        // C. Look for matching entries in previous declarations (variables/parameters)
        for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
        {
            if (it->name == firstToken || (it->fullName.length() >= firstToken.length() &&
                                           it->fullName.substr(it->fullName.length() - firstToken.length()) == firstToken))
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
                        {
                            deduced = deduced.substr(startAngle + 1, endAngle - startAngle - 1);
                        }
                    }
                    return deduced;
                }
            }
        }

        // D. Verify native engine types
        if (nativeEng)
        {
            if (nativeEng->GetTypeInfoByName(firstToken.c_str()))
            {
                return firstToken;
            }
        }
        return "auto";
    };

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
                        pToks.push_back(allTokens[idxP]);
                    if (!pToks.empty() && IsWord(pToks.back().text) && !reservedKeywords.count(pToks.back().text))
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
                        funcParams.push_back({nTok.text, ptStr});
                    }
                    pToks.clear();
                }
                else
                    pToks.push_back(allTokens[idxP]);
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

            for (const auto &fp : funcParams)
                declarations.push_back({fp.pName, fp.pName, "parameter", fp.pType + " " + fp.pName, allTokens[nameIdx].startPos, functionEndPos});

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
                    std::string lTxt = allTokens[lambdaIdxScan].text;

                    // Support internal foreach within the lambda body
                    if (lTxt == "foreach")
                    {
                        size_t loopIdx = lambdaIdxScan + 1;
                        if (loopIdx < endBraceTokenIdx && allTokens[loopIdx].text == "(")
                        {
                            size_t nextIdx = loopIdx + 1;
                            std::string loopTypeStr = "";
                            if (ParseType(loopIdx + 1, nextIdx, loopTypeStr))
                            {
                                if (nextIdx < endBraceTokenIdx && IsWord(allTokens[nextIdx].text) && !reservedKeywords.count(allTokens[nextIdx].text))
                                {
                                    std::string loopVarName = allTokens[nextIdx].text;

                                    std::string containerName = "";
                                    if (nextIdx + 1 < endBraceTokenIdx && allTokens[nextIdx + 1].text == "in")
                                    {
                                        if (nextIdx + 2 < endBraceTokenIdx && IsWord(allTokens[nextIdx + 2].text))
                                        {
                                            containerName = allTokens[nextIdx + 2].text;
                                        }
                                    }

                                    if (loopTypeStr.find("auto") != std::string::npos && !containerName.empty())
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
                                    declarations.push_back({loopVarName, loopVarName, "local_variable", loopTypeStr + " " + loopVarName, allTokens[loopIdx].startPos, functionEndPos});
                                }
                            }
                        }
                        lambdaIdxScan++;
                        continue;
                    }

                    // Support traditional internal for within the lambda body
                    if (lTxt == "for")
                    {
                        size_t loopIdx = lambdaIdxScan + 1;
                        if (loopIdx < endBraceTokenIdx && allTokens[loopIdx].text == "(")
                        {
                            size_t nextIdx = loopIdx + 1;
                            std::string loopTypeStr = "";
                            if (ParseType(loopIdx + 1, nextIdx, loopTypeStr))
                            {
                                if (nextIdx < endBraceTokenIdx && IsWord(allTokens[nextIdx].text) && !reservedKeywords.count(allTokens[nextIdx].text))
                                {
                                    std::string loopVarName = allTokens[nextIdx].text;
                                    size_t nameTokenIdx = nextIdx;

                                    if (loopTypeStr.find("auto") != std::string::npos)
                                    {
                                        if (nameTokenIdx + 1 < endBraceTokenIdx && allTokens[nameTokenIdx + 1].text == "=")
                                        {
                                            std::string deduced = DeduceTypeFromRHS(nameTokenIdx + 2);
                                            if (deduced != "auto")
                                            {
                                                loopTypeStr = deduced;
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

                    std::string typeStr = "";
                    size_t nextIdx = lambdaIdxScan;

                    if (ParseType(lambdaIdxScan, nextIdx, typeStr))
                    {
                        if (nextIdx <= lambdaIdxScan)
                        {
                            lambdaIdxScan++;
                            continue;
                        }
                        std::string baseType = allTokens[lambdaIdxScan].text;

                        if (baseType == "return" || baseType == "if" || baseType == "for" || baseType == "foreach" || baseType == "while" ||
                            baseType == "break" || baseType == "continue" || baseType == "switch" || baseType == "case" ||
                            baseType == "default" || baseType == "cast" || baseType == "try" || baseType == "catch" ||
                            baseType == "delete" || baseType == "throw")
                        {
                            lambdaIdxScan = nextIdx;
                            continue;
                        }

                        size_t innerScan = nextIdx;
                        bool advanced = false;
                        while (innerScan < endBraceTokenIdx && IsWord(allTokens[innerScan].text) && !reservedKeywords.count(allTokens[innerScan].text))
                        {
                            size_t nameIdx = innerScan;
                            std::string entityName = allTokens[nameIdx].text;

                            if (nameIdx + 1 < allTokens.size() && (allTokens[nameIdx + 1].text == ";" || allTokens[nameIdx + 1].text == "=" || allTokens[nameIdx + 1].text == "," || allTokens[nameIdx + 1].text == "[" || allTokens[nameIdx + 1].text == ")"))
                            {
                                std::string finalTypeStr = typeStr;
                                if (typeStr.find("auto") != std::string::npos)
                                {
                                    if (allTokens[nameIdx + 1].text == "=" && nameIdx + 2 < allTokens.size())
                                    {
                                        std::string deduced = DeduceTypeFromRHS(nameIdx + 2);
                                        if (deduced != "auto")
                                        {
                                            finalTypeStr = deduced;
                                            if (typeStr.find('@') != std::string::npos && finalTypeStr.find('@') == std::string::npos)
                                                finalTypeStr += "@";
                                        }
                                    }
                                }

                                declarations.push_back({entityName, entityName, "local_variable", finalTypeStr + " " + entityName, allTokens[bodyBraceIdx].startPos, functionEndPos});

                                size_t searchComma = nameIdx + 1;
                                int sqDepth = 0;
                                while (searchComma < endBraceTokenIdx)
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
}

/**
 * @brief Validates the targeted token using extracted semantic metadata to resolve hover text.
 * @param targetIdx The index of the targeted token.
 * @return A markdown-formatted string containing the hover disclosure, or an empty string if unresolved.
 */
std::string HoverHandler::SemanticValidationPass(int targetIdx)
{
    if (targetIdx == -1)
        return "";

    std::string hoverResult = "";
    TokenInfo targetTok = allTokens[targetIdx];
    bool isMemberAccess = (targetIdx >= 1 && allTokens[targetIdx - 1].text == ".");

    // Lambda auxiliar ultra-robusto para detectar e inyectar firmas de funcdef (locales o nativas)
    auto EnhanceIfFuncdef = [&](const std::string &hoverText) -> std::string
    {
        std::string baseHover = hoverText;
        if (!nativeEng)
            return baseHover;

        std::string cleanedText = baseHover;
        size_t startPos = 0;
        if (cleanedText.find("private ") == 0)
            startPos = 8;
        else if (cleanedText.find("protected ") == 0)
            startPos = 10;
        else if (cleanedText.find("public ") == 0)
            startPos = 7;

        size_t firstSpace = cleanedText.find(' ', startPos);
        if (firstSpace != std::string::npos)
        {
            std::string typeName = cleanedText.substr(startPos, firstSpace - startPos);
            if (!typeName.empty() && typeName.back() == '@')
                typeName.pop_back();

            std::string paramsSuffix = "";

            for (const auto &d : declarations)
            {
                if (d.type == "funcdef" && d.name == typeName)
                {
                    size_t paren = d.hoverText.find('(');
                    if (paren != std::string::npos)
                    {
                        paramsSuffix = d.hoverText.substr(paren);
                        break;
                    }
                }
            }

            if (paramsSuffix.empty())
            {
                asIScriptModule *mod = nativeEng->GetModule("LSPModule");
                asITypeInfo *fdefInfo = mod ? mod->GetTypeInfoByName(typeName.c_str()) : nullptr;
                if (!fdefInfo)
                {
                    fdefInfo = nativeEng->GetTypeInfoByName(typeName.c_str());
                }

                if (fdefInfo && (fdefInfo->GetFlags() & asOBJ_FUNCDEF))
                {
                    asIScriptFunction *fdefFunc = fdefInfo->GetFuncdefSignature();
                    if (fdefFunc)
                    {
                        std::string fullFuncDecl = fdefFunc->GetDeclaration(true, true);
                        size_t parenPos = fullFuncDecl.find('(');
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
    };

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
            objName = allTokens[backIdx].text;

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
                    break;
                cleanTypeName += c;
            }

            std::vector<std::string> searchHierarchy = {cleanTypeName};
            if (classInheritanceMapper.count(cleanTypeName))
            {
                for (const auto &baseClass : classInheritanceMapper[cleanTypeName])
                    searchHierarchy.push_back(baseClass);
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
                            hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(EnhanceIfFuncdef(decl.hoverText)));
                            methodResolved = true;
                            break;
                        }
                    }
                }

                if (methodResolved)
                    break;

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
                                    declStr.erase(tppos, 5);

                                if (declStr.find(cleanTypeName + "::") == std::string::npos)
                                {
                                    size_t namePos = declStr.find(targetTok.text + "(");
                                    if (namePos != std::string::npos)
                                        declStr.insert(namePos, objType + "::");
                                }

                                // =========================================================================
                                // NUEVO: REEMPLAZAR EL TIPO GENÉRICO 'T' CON EL ARGUMENTO REAL DE LA PLANTILLA
                                // =========================================================================
                                size_t openBracket = objType.find('<');
                                size_t closeBracket = objType.rfind('>');
                                if (openBracket != std::string::npos && closeBracket != std::string::npos && closeBracket > openBracket)
                                {
                                    std::string templateArg = objType.substr(openBracket + 1, closeBracket - openBracket - 1);
                                    size_t tPos = 0;
                                    while ((tPos = declStr.find('T', tPos)) != std::string::npos)
                                    {
                                        // Validación de palabra completa (evita romper nombres como uint, int, float, etc.)
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
                                // =========================================================================

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
        if (targetTok.text != "this" && targetTok.text != "super" && targetTok.text != "function")
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
                break;
        }

        bool isFollowedByParen = (targetIdx + 1 < (int)allTokens.size() && allTokens[targetIdx + 1].text == "(");
        bool isPrecededByTilde = (targetIdx > 0 && allTokens[targetIdx - 1].text == "~");

        auto MatchesQual = [&](const DeclInfo &decl) -> bool
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
        };

        for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
        {
            const auto &decl = *it;
            if ((decl.type == "local_variable" || decl.type == "parameter" || decl.type == "global_variable") && decl.name == targetTok.text)
            {
                if (decl.type == "global_variable" || (targetTok.startPos >= decl.startPos && targetTok.startPos <= decl.endPos))
                {
                    hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(EnhanceIfFuncdef(decl.hoverText)));
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
                    hoverResult = fmt::format("```cpp\n{}\n```", CleanSignature(EnhanceIfFuncdef(decl.hoverText)));
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
                    hoverResult = fmt::format("```cpp\nclass {}\n```", tInfo->GetName());
            }
        }
    }

    return hoverResult;
}

/**
 * @brief Orchestrates the multi-pass compilation and processing pipeline to yield the final hover result.
 * @return A JSON object containing the markdown content payload, or nullptr if no hover information is resolved.
 */
json HoverHandler::Process()
{
    TokenizePass();
    int targetIdx = FindTargetTokenIdx();

    StructuralParsingPass();
    ExtractDeclarationsPass();
    ExtractLambdasPass();

    std::string resultMarkdown = SemanticValidationPass(targetIdx);

    if (!resultMarkdown.empty())
    {
        return {{"contents", {{"kind", "markdown"}, {"value", resultMarkdown}}}};
    }
    return nullptr;
}