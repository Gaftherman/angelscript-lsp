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
     * @brief Context and immutable input parameters for a Document Highlight request.
     */
    struct DocumentHighlightRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        lsp::Position position;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
    };

    /**
     * @brief List of document highlights in the current document.
     */
    using DocumentHighlightResult = std::vector<lsp::DocumentHighlight>;

    /**
     * @brief Resolves all read/write occurrences of the symbol under cursor in the current document.
     * @param request Immutable context for document highlight lookup.
     * @return Optional vector of DocumentHighlight items; nullopt if cursor symbol cannot be resolved.
     */
    std::optional<DocumentHighlightResult> GetDocumentHighlights(const DocumentHighlightRequest &request);
}
