#pragma once

#include "analysis/SymbolTable.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a workspace symbol request.
     */
    struct WorkspaceSymbolRequest
    {
        std::string_view query;
        const analysis::SymbolTable &symbolTable;
        size_t maxResults = 100;
    };

    /**
     * @brief Result list containing flat symbol information across the workspace.
     */
    using WorkspaceSymbolResult = std::vector<lsp::SymbolInformation>;

    /**
     * @brief Searches indexed symbols in the SymbolTable matching the query string.
     * @param request Immutable context for workspace symbol search.
     * @return Optional vector of SymbolInformation objects matching the query.
     */
    std::optional<WorkspaceSymbolResult> GetWorkspaceSymbols(const WorkspaceSymbolRequest &request);
}
