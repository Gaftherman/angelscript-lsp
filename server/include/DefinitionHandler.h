/**
 * @file DefinitionHandler.h
 * @brief Context-aware Language Server Protocol definition mapping controller layout.
 * @author AngelScript LSP Team
 */

#ifndef DEFINITION_HANDLER_H
#define DEFINITION_HANDLER_H

#include "HoverHandler.h"
#include <nlohmann/json.hpp>
#include <angelscript.h>

using json = nlohmann::json;

namespace DefinitionUtils
{
    /**
     * @class DefinitionResolver
     * @brief Specialized resolver parsing native tokens to map symbol references onto origin definitions.
     */
    class DefinitionResolver : public LspCore::BaseResolver
    {
    public:
        /**
         * @brief Constructor initializing tracking context fields.
         */
        DefinitionResolver(asIScriptEngine *engine, std::string_view sourceCode, int line, int character);

        virtual ~DefinitionResolver() = default;

        /**
         * @brief Scans declarations to return absolute source character limits matching target constraints.
         * @param targetIdx The index of the token under inspection.
         * @param outLength Reference parameter filled with the token text length.
         * @return The flat absolute character offset position of the symbol's declaration.
         */
        size_t ResolveDefinitionPosition(int targetIdx, size_t &outLength);
    };

    /**
     * @brief Assembles a standard LSP Location object representation in JSON format.
     * @param uri Document target absolute reference path string.
     * @param sourceCode Active loaded character stream data layout.
     * @param offset Absolute flat character coordinates position layout index.
     * @param wordLength Spanning token character allocation length metric.
     * @return Generated json object tracking bounds positioning layout metrics.
     */
    json CreateLocationJson(std::string_view uri, std::string_view sourceCode, size_t offset, size_t wordLength);
}

/**
 * @class DefinitionHandler
 * @brief Resolves definition coordinates by utilizing token harvest hierarchies matching the hover engine.
 */
class DefinitionHandler
{
public:
    DefinitionHandler() = default;
    ~DefinitionHandler() = default;

    /**
     * @brief Evaluates code state context to map the target symbol declaration location.
     * @param engine Pointer to the active core AngelScript compiler engine instance framework.
     * @param request Incoming client JSON-RPC definition request payload framing.
     * @param sourceCode Multi-line text buffer layout matching the active script document.
     * @return A json Location object tracking absolute file URI and coordinates range, or nullptr.
     */
    static json HandleDefinitionRequest(asIScriptEngine *engine, const json &request, std::string_view sourceCode);
};

#endif // DEFINITION_HANDLER_H