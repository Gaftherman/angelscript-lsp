#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <optional>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a definition request.
     */
    struct DefinitionRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
        lsp::Position position;
    };

    /**
     * @brief Resolves definition locations for symbol under cursor.
     * @param request Immutable context for definition lookup.
     * @return Optional vector of Locations; nullopt or empty if not resolved.
     */
    std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest &request);

    /**
     * @brief Resolves type definition locations for symbol under cursor.
     * @param request Immutable context for type definition lookup.
     * @return Optional vector of Locations pointing to type declarations.
     */
    std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest &request);
}
