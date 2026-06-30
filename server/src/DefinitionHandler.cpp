/**
 * @file DefinitionHandler.cpp
 * @brief Implements full semantic validation tracing for definitions using the base core extraction pipeline.
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
    DefinitionResolver::DefinitionResolver(asIScriptEngine *engine, std::string_view sourceCode, int line, int character)
        : BaseResolver(engine, sourceCode, line, character)
    {
    }

    size_t DefinitionResolver::ResolveDefinitionPosition(int targetIdx, size_t &outLength)
    {
        outLength = 0;

        if (targetIdx == -1)
            return std::string_view::npos;

        LspCore::TokenInfo targetTok = allTokens[static_cast<size_t>(targetIdx)];
        bool isMemberAccess = (targetIdx >= 1 && allTokens[static_cast<size_t>(targetIdx - 1)].text == ".");

        // Semantic analysis mapping of a member property/method access: variable.property
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

        // Semantic lookup mapping of flat code identifiers: variables, functions, lambdas
        if (targetTok.tokenClass == asTC_IDENTIFIER)
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

            bool isFollowedByParen = (targetIdx + 1 < static_cast<int>(allTokens.size()) && allTokens[static_cast<size_t>(targetIdx + 1)].text == "(");
            bool isPrecededByTilde = (targetIdx > 0 && allTokens[static_cast<size_t>(targetIdx - 1)].text == "~");

            for (auto it = declarations.rbegin(); it != declarations.rend(); ++it)
            {
                if ((it->type == "local_variable" || it->type == "parameter" || it->type == "global_variable") && it->name == targetTok.text)
                {
                    if (it->type == "global_variable" || (targetTok.startPos >= it->startPos && targetTok.startPos <= it->endPos))
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
} // namespace DefinitionUtils

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

    std::string uri = textDoc[std::string(KEY_URI)].get<std::string>();
    int line = pos[std::string(KEY_LINE)].get<int>();
    int character = pos[std::string(KEY_CHARACTER)].get<int>();

    // Abstraction layer parsing through BaseResolver instantiation (No Regex matching)
    DefinitionUtils::DefinitionResolver resolver(engine, sourceCode, line, character);
    resolver.RunParserPipeline();

    int targetIdx = resolver.FindTargetTokenIdx();
    size_t matchLength = 0;
    size_t defOffset = resolver.ResolveDefinitionPosition(targetIdx, matchLength);

    if (defOffset != std::string_view::npos && matchLength > 0)
    {
        return DefinitionUtils::CreateLocationJson(uri, sourceCode, defOffset, matchLength);
    }

    return nullptr;
}