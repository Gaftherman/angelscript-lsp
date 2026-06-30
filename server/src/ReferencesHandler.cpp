/**
 * @file ReferencesHandler.cpp
 * @brief Implements comprehensive lexical scope resolution and tracking for identifier reference queries with advanced tracking telemetry.
 */

#include "ReferencesHandler.h"
#include <algorithm>
#include <string>

namespace
{
    constexpr std::string_view KEY_PARAMS = "params";
    constexpr std::string_view KEY_TEXT_DOCUMENT = "textDocument";
    constexpr std::string_view KEY_URI = "uri";
    constexpr std::string_view KEY_POSITION = "position";
    constexpr std::string_view KEY_LINE = "line";
    constexpr std::string_view KEY_CHARACTER = "character";
}

namespace ReferenceUtils
{
    ReferencesResolver::ReferencesResolver(asIScriptEngine *engine, std::string_view sourceCode, int line, int character, std::function<void(std::string_view, int)> logCallback)
        : BaseResolver(engine, sourceCode, line, character), logger(logCallback)
    {
    }

    json ReferencesResolver::ResolveReferences(int targetIdx, std::string_view uri)
    {
        json referencesRegistry = json::array();

        // MILESTONE 2: Validar resultado inmediato del lookup de tokens sobre la posición del cursor
        if (targetIdx == -1)
        {
            if (logger)
            {
                logger("[References Debug] [ERROR] No token found at requested coordinates.", 1);
            }

            return referencesRegistry;
        }

        LspCore::TokenInfo targetTok = allTokens[static_cast<size_t>(targetIdx)];
        bool isMemberAccess = (targetIdx >= 1 && allTokens[static_cast<size_t>(targetIdx - 1)].text == ".");

        if (logger)
        {
            logger("[References Debug] Active token identified: '" + std::string(targetTok.text) + "' [Type: " + std::to_string(targetTok.tokenClass) + "]", 4);
        }

        LspCore::DeclInfo targetDecl;
        bool foundAnchor = false;

        // MILESTONE 3: Resolución semántica del Ancla de definición sin Regex
        if (isMemberAccess && targetTok.tokenClass == asTC_IDENTIFIER)
        {
            std::string objName = "";
            int backIdx = targetIdx - 2;

            if (backIdx >= 0 && allTokens[static_cast<size_t>(backIdx)].text == "]")
            {
                int depth = 1;
                backIdx--;

                while (backIdx >= 0 && depth > 0)
                {
                    if (allTokens[static_cast<size_t>(backIdx)].text == "]")
                        depth++;
                    else if (backIdx >= 0 && allTokens[static_cast<size_t>(backIdx)].text == "[")
                        depth--;

                    backIdx--;
                }
            }

            if (backIdx >= 0 && IsWord(allTokens[static_cast<size_t>(backIdx)].text))
            {
                objName = std::string(allTokens[static_cast<size_t>(backIdx)].text);
            }

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
                    const auto &bases = classInheritanceMapper[std::string(cleanTypeName)];

                    for (size_t b = 0; b < bases.size(); ++b)
                    {
                        searchHierarchy.push_back(bases[b]);
                    }
                }

                for (size_t s = 0; s < searchHierarchy.size(); ++s)
                {
                    const std::string &typeHierarchyStr = searchHierarchy[s];

                    for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                    {
                        if (it->name == targetTok.text && IsCallableOrFieldSemantic(it->type) && it->fullName.find(typeHierarchyStr + "::") != std::string::npos)
                        {
                            targetDecl = *it;
                            foundAnchor = true;

                            if (logger)
                            {
                                logger("[References Debug] Recognized active token as an encapsulated member usage. Anchor resolved to: " + targetDecl.fullName, 3);
                            }

                            break;
                        }
                    }

                    if (foundAnchor)
                        break;
                }
            }
        }
        else if (targetTok.tokenClass == asTC_IDENTIFIER)
        {
            std::string fullQualName = std::string(targetTok.text);
            int left = targetIdx - 1;

            while (left >= 0 && allTokens[static_cast<size_t>(left)].text == "::")
            {
                if (left > 0 && IsWord(allTokens[static_cast<size_t>(left - 1)].text))
                {
                    fullQualName = std::string(allTokens[static_cast<size_t>(left - 1)].text) + "::" + fullQualName;
                    left -= 2;
                }
                else
                {
                    break;
                }
            }

            for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                if ((it->type == "local_variable" || it->type == "parameter" || it->type == "global_variable") && it->name == targetTok.text)
                {
                    if (it->type == "global_variable" || (targetTok.startPos >= it->startPos && targetTok.startPos <= it->endPos))
                    {
                        targetDecl = *it;
                        foundAnchor = true;

                        if (logger)
                        {
                            logger("[References Debug] Recognized active token as a flat identifier usage. Anchor resolved to: " + targetDecl.name + " [Scope Type: " + targetDecl.type + "]", 3);
                        }

                        break;
                    }
                }
            }

            if (!foundAnchor)
            {
                for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                {
                    if (MatchesQual(*it, fullQualName))
                    {
                        targetDecl = *it;
                        foundAnchor = true;

                        if (logger)
                        {
                            logger("[References Debug] Recognized active token directly at its primary root declaration context. Anchor resolved.", 3);
                        }

                        break;
                    }
                }
            }
        }

        if (!foundAnchor)
        {
            if (logger)
            {
                logger("[References Debug] [WARNING] Failed to resolve declaration anchor for token '" + std::string(targetTok.text) + "'.", 2);
            }

            return referencesRegistry;
        }

        // STEP B: Mapeo bidireccional sobre el flujo secuencial de tokens del documento
        for (size_t k = 0; k < allTokens.size(); ++k)
        {
            const auto &tok = allTokens[k];

            if (tok.tokenClass == asTC_IDENTIFIER && tok.text == targetDecl.name)
            {
                size_t tokPos = tok.startPos;
                bool isLinkedReference = false;

                if (targetDecl.type == "local_variable" || targetDecl.type == "parameter")
                {
                    if (tokPos >= targetDecl.startPos && tokPos <= targetDecl.endPos)
                    {
                        bool isShadowed = false;

                        for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
                        {
                            if (it->name == targetDecl.name && (it->type == "local_variable" || it->type == "parameter") && it->defPos != targetDecl.defPos)
                            {
                                if (tokPos >= it->defPos && tokPos <= it->endPos && it->defPos > targetDecl.defPos)
                                {
                                    isShadowed = true;
                                    break;
                                }
                            }
                        }

                        if (!isShadowed)
                        {
                            isLinkedReference = true;
                        }
                    }
                }
                else
                {
                    isLinkedReference = true;
                }

                if (isLinkedReference)
                {
                    referencesRegistry.push_back(CreateLocationJson(uri, originalText, tok.startPos, tok.text.length()));
                }
            }
        }

        // MILESTONE 4: Registro final del tamaño del arreglo y serialización
        if (logger)
        {
            logger("[References Debug] Registry lookup completed. Found " + std::to_string(referencesRegistry.size()) + " reference locations.", 4);
        }

        if (referencesRegistry.empty() && logger)
        {
            logger("[References Debug] [ERROR] Returning an empty JSON array to client.", 1);
        }

        return referencesRegistry;
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
} // namespace ReferenceUtils

json ReferencesHandler::HandleReferencesRequest(asIScriptEngine *engine, const json &request, std::string_view sourceCode, std::function<void(std::string_view, int)> logCallback)
{
    if (!request.contains(KEY_PARAMS))
    {
        if (logCallback)
        {
            logCallback("[References Debug] [ERROR] Missing parameters in request payload.", 1);
        }

        return json::array();
    }

    const auto &params = request[std::string(KEY_PARAMS)];

    if (!params.contains(KEY_TEXT_DOCUMENT) || !params.contains(KEY_POSITION))
        return json::array();

    const auto &textDoc = params[std::string(KEY_TEXT_DOCUMENT)];
    const auto &pos = params[std::string(KEY_POSITION)];

    if (!textDoc.contains(KEY_URI) || !pos.contains(KEY_LINE) || !pos.contains(KEY_CHARACTER))
        return json::array();

    std::string uri = textDoc[std::string(KEY_URI)].get<std::string>();
    int line = pos[std::string(KEY_LINE)].get<int>();
    int character = pos[std::string(KEY_CHARACTER)].get<int>();

    // MILESTONE 1: Solicitud inicial recibida con metadatos de coordenadas completas
    if (logCallback)
    {
        logCallback("[References Debug] Request received for URI: " + std::string(uri) + " at Line: " + std::to_string(line) + ", Char: " + std::to_string(character), 4);
    }

    ReferenceUtils::ReferencesResolver resolver(engine, sourceCode, line, character, logCallback);
    resolver.RunParserPipeline();

    int targetIdx = resolver.FindTargetTokenIdx();

    return resolver.ResolveReferences(targetIdx, uri);
}