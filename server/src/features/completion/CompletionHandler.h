#pragma once

#include "analysis/completion/CompletionEngine.h"
#include "analysis/completion/CompletionTypes.h"
#include "analysis/index/SymbolTable.h"

#include <lsp/types.h>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Computes the completion list for a query and maps it to LSP completion items.
 *        Context detection, candidate collection and ranking happen in `analysis::completion`; this layer
 *        only converts kinds, snippet flags, documentation, resolve keys and include-path edits.
 * @param[in] query Document, cursor and lookups for this completion.
 * @return Completion items in the engine's sort order; empty where no completion applies.
 */
std::vector<lsp::CompletionItem> GetCompletion(const analysis::completion::CompletionQuery& query);

/**
 * @brief Context for filling in the documentation of one completion item on demand.
 */
struct CompletionResolveRequest
{
    /**
     * @brief The item the client picked, carrying the qualified name GetCompletion put in `data`.
     */
    const lsp::CompletionItem& item;

    const analysis::SymbolTable& symbolTable; ///< Workspace symbols used to find the declarations.

    /**
     * @brief Reads the text of the file that declares the symbol; injected because only the host knows
     *        which documents are open and how long their text stays alive.
     */
    analysis::completion::DocumentTextReader readDocument = {};
};

/**
 * @brief Fills in the documentation of a completion item, on demand.
 *        Extracting a doc comment means finding the declaring file and re-reading the lines above the
 *        declaration, which is too costly to do for every item when the list is built.
 * @param[in] request Item to complete plus the lookups needed to do it.
 * @return The item with markdown documentation attached, or unchanged when it already has documentation,
 *         carries no string `data`, or no declaration has a comment.
 */
lsp::CompletionItem ResolveCompletionItem(const CompletionResolveRequest& request);

} // namespace angel_lsp::features
