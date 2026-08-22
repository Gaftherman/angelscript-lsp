#pragma once

#include "analysis/SymbolTable.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a document symbol request.
     */
    struct DocumentSymbolRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
    };

    /**
     * @brief Result list containing hierarchical document symbols for outline navigation.
     */
    using DocumentSymbolResult = std::vector<lsp::DocumentSymbol>;

    /**
     * @brief Generates hierarchical document symbols (classes, methods, fields, namespaces, functions, enums)
     *        from the Tree-Sitter AST of the given document.
     * @param request Immutable context for document symbol extraction.
     * @return Optional vector of DocumentSymbol objects; nullopt if document cannot be parsed.
     */
    std::optional<DocumentSymbolResult> GetDocumentSymbols(const DocumentSymbolRequest &request);
}
