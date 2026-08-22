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
     * @brief Context and input parameters for a Find References request.
     */
    struct ReferencesRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        lsp::Position position;
        bool includeDeclaration = true;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
    };

    using ReferencesResult = std::vector<lsp::Location>;

    /**
     * @brief Resolves all declaration and reference occurrences of any target symbol
     *        (local variables, function parameters, class fields/methods, global declarations)
     *        across documents and lexical scopes.
     * @param request Immutable context for references lookup.
     * @return Optional vector of Locations; nullopt if symbol cannot be resolved or no references found.
     */
    std::optional<ReferencesResult> GetReferences(const ReferencesRequest &request);
}
