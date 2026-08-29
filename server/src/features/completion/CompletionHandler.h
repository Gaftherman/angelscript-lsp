#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
#include "config/ServerConfig.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <functional>
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

        /**
         * @brief Whether the client renders `${1:T}` placeholders rather than printing them.
         *
         * Read from `textDocument.completion.completionItem.snippetSupport` at initialize. It has
         * to be asked rather than assumed: a template class completes to `array<${1:T}>`, and a
         * client without snippet support would insert those six characters literally.
         */
        bool snippetSupport = false;
    };

    /**
     * @brief Computes completion items based on lexical scope and member access context.
     * @param request Immutable context for completion.
     * @return Vector of CompletionItem objects.
     */
    std::vector<lsp::CompletionItem> GetCompletion(const CompletionRequest &request);

    /**
     * @brief Context for filling in the details of one completion item.
     */
    struct CompletionResolveRequest
    {
        /** @brief The item the client picked, carrying the identity GetCompletion put on `data`. */
        const lsp::CompletionItem &item;

        const analysis::SymbolTable &symbolTable;

        /**
         * @brief Reads a document's text by URI, or returns nullptr when the server holds none.
         *
         * Injected rather than taken as a map: the documentation lives in the file that declares
         * the symbol, which is rarely the file completion was invoked in, and only the server
         * knows which documents it currently holds text for.
         */
        std::function<const std::string *(const std::string &)> readDocument;
    };

    /**
     * @brief Fills in the documentation of a completion item, on demand.
     *
     * Extracting a doc comment means finding the declaring file and re-scanning the lines above the
     * declaration. Doing that for every item of every completion request costs far more than doing
     * it once for the item the user actually highlighted, which is what this exists for.
     *
     * @param request Item to complete plus the lookups needed to do it.
     * @return The item with documentation attached, or unchanged when there is none to add.
     */
    lsp::CompletionItem ResolveCompletionItem(const CompletionResolveRequest &request);
}
