#pragma once

#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and immutable input parameters for a Document Highlight request.
 */
struct DocumentHighlightRequest
{
    const std::string& uri;
    const std::string& sourceCode;
    TSTree* tree = nullptr;
    lsp::Position position;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
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
std::optional<DocumentHighlightResult> GetDocumentHighlights(const DocumentHighlightRequest& request);
} // namespace angel_lsp::features
