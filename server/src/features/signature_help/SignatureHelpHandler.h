#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <string>
#include <optional>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a signature help request.
     */
    struct SignatureHelpRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
        lsp::Position position;
    };

    /**
     * @brief Computes signature help info and active parameter index for function call.
     * @param request Immutable context for signature help.
     * @return Optional SignatureHelp struct; nullopt if position is not in a function call.
     */
    std::optional<lsp::SignatureHelp> GetSignatureHelp(const SignatureHelpRequest &request);
}
