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
     * @brief Context and immutable input parameters for an Inlay Hint request.
     */
    struct InlayHintRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        lsp::Range range;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
    };

    /**
     * @brief List of inlay hints to display in the editor.
     */
    using InlayHintResult = std::vector<lsp::InlayHint>;

    /**
     * @brief Computes inlay hints (parameter names for function/method calls,
     *        and type deduction for auto variables) within the requested document range.
     * @param request Immutable context for inlay hint computation.
     * @return Optional vector of InlayHint items; nullopt if document cannot be parsed or no hints found.
     */
    std::optional<InlayHintResult> GetInlayHints(const InlayHintRequest &request);
}
