/**
 * @file ReferencesHandler.h
 * @brief Context-aware Language Server Protocol references handler with advanced trace telemetry.
 * @author AngelScript LSP Team
 */

#ifndef REFERENCES_HANDLER_H
#define REFERENCES_HANDLER_H

#include "HoverHandler.h"
#include <nlohmann/json.hpp>
#include <angelscript.h>
#include <functional>
#include <string_view>

using json = nlohmann::json;

namespace ReferenceUtils
{
    /**
     * @class ReferencesResolver
     * @brief Lexical-scope aware resolver implementing references search algorithms with active logging.
     */
    class ReferencesResolver : public LspCore::BaseResolver
    {
    public:
        std::function<void(std::string_view, int)> logger;

        /**
         * @brief Constructor initializing tracking context fields and logging callback.
         */
        ReferencesResolver(asIScriptEngine *engine, std::string_view sourceCode, int line, int character, std::function<void(std::string_view, int)> logCallback);

        virtual ~ReferencesResolver() = default;

        /**
         * @brief Scans all tokens linking scoped IDENTIFIER references back to their definition anchor with trace notification.
         */
        json ResolveReferences(int targetIdx, std::string_view uri);
    };

    /**
     * @brief Assembles a standard LSP Location object representation in JSON format.
     */
    json CreateLocationJson(std::string_view uri, std::string_view sourceCode, size_t offset, size_t wordLength);
}

/**
 * @class ReferencesHandler
 * @brief Interfaces directly with the routing pipeline to answer textDocument/references queries with diagnostics.
 */
class ReferencesHandler
{
public:
    ReferencesHandler() = default;
    ~ReferencesHandler() = default;

    /**
     * @brief Evaluates code state context to map all symbol reference locations injecting telemetry logs.
     */
    static json HandleReferencesRequest(asIScriptEngine *engine, const json &request, std::string_view sourceCode, std::function<void(std::string_view, int)> logCallback = nullptr);
};

#endif // REFERENCES_HANDLER_H