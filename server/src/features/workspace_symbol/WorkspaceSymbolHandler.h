#pragma once

#include "analysis/index/SymbolTable.h"
#include "utils/Constants.h"

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
    std::string_view query{};
    const analysis::SymbolTable& symbolTable;
    size_t maxResults = constants::limits::DefaultWorkspaceSymbolResults;
};

/**
 * @brief Result list containing flat symbol information across the workspace.
 */
using WorkspaceSymbolResult = std::vector<lsp::SymbolInformation>;

/**
 * @brief Searches indexed symbols in the SymbolTable matching the query string.
 * @param[in] request Immutable context for workspace symbol search.
 * @return Optional vector of SymbolInformation objects matching the query.
 */
[[nodiscard]] std::optional<WorkspaceSymbolResult> GetWorkspaceSymbols(const WorkspaceSymbolRequest& request);

/**
 * @brief Symbol descriptor scored by fuzzy matcher for workspace symbol search.
 */
struct ScoredSymbol
{
    int score = 0;
    lsp::SymbolInformation info;
};

/**
 * @brief Fuzzy matching score and highlighted character intervals for a symbol name.
 */
struct MatchResult
{
    bool matched = false;
    int score = 0;
};
} // namespace angel_lsp::features
