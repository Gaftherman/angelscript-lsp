#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>

#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a Code Lens request.
     */
    struct CodeLensRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
    };

    /**
     * @brief Context and input parameters for resolving a Code Lens item.
     */
    struct CodeLensResolveRequest
    {
        const lsp::CodeLens &codeLens;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
    };

    /**
     * @brief Computes code lenses (such as reference counts and interface implementations) for a document.
     * @param request Immutable context for code lens generation.
     * @return List of code lenses for classes, interfaces, methods, and functions.
     */
    std::optional<std::vector<lsp::CodeLens>> GetCodeLenses(const CodeLensRequest &request);

    /**
     * @brief Resolves additional command or details for a given Code Lens.
     * @param request Immutable context for code lens resolution.
     * @return Resolved Code Lens.
     */
    std::optional<lsp::CodeLens> ResolveCodeLens(const CodeLensResolveRequest &request);
}
