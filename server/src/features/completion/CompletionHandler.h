#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
#include "config/ServerConfig.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <string>
#include <vector>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for an auto-completion request.
     */
    struct CompletionRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
        lsp::Position position;
        const config::ServerConfig *config = nullptr;
    };

    /**
     * @brief Computes completion items based on lexical scope and member access context.
     * @param request Immutable context for completion.
     * @return Vector of CompletionItem objects.
     */
    std::vector<lsp::CompletionItem> GetCompletion(const CompletionRequest &request);
}
