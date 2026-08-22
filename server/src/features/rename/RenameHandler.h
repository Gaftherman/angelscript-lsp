#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <optional>
#include <unordered_set>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a Prepare Rename request.
     */
    struct PrepareRenameRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        lsp::Position position;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
        const std::unordered_set<std::string> &predefinedUris;
    };

    /**
     * @brief Context and input parameters for a Rename execution request.
     */
    struct RenameRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        lsp::Position position;
        std::string newName;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
        const std::unordered_set<std::string> &predefinedUris;
    };

    /**
     * @brief Validates whether the symbol under cursor can be safely renamed and returns its source range.
     * @param request Immutable context for prepare rename validation.
     * @return Optional PrepareRenameResult containing range and placeholder text; nullopt if invalid.
     */
    std::optional<lsp::PrepareRenameResult> PrepareRename(const PrepareRenameRequest &request);

    /**
     * @brief Generates a WorkspaceEdit with TextEdit blocks replacing all occurrences of the renamed symbol
     *        across affected files without breaking unaffected or shadowed identifiers.
     * @param request Immutable context for rename execution.
     * @return Optional WorkspaceEdit; nullopt if validation failed or no edits produced.
     */
    std::optional<lsp::WorkspaceEdit> Rename(const RenameRequest &request);
}
