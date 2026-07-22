/**
 * @file CompletionHandler.h
 * @brief Handler for LSP textDocument/completion auto-suggestion requests.
 * @ingroup Features
 */

#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

#include "analysis/SymbolTable.h"

class asIScriptEngine;

namespace angel_lsp::features::completion
{

    /**
     * @brief Processes an LSP Completion request to provide context-aware suggestions.
     *
     * @param[in] req The completion request parameters (including document URI and cursor position).
     * @param[in] doc The parsed Document containing Tree-Sitter AST and source code.
     * @param[in] table The SymbolTable containing resolved symbols for the document.
     * @param[in] engine Optional pointer to the AngelScript engine for predefined types.
     * @return lsp::requests::TextDocument_Completion::Result The list of completion items.
     * @note Thread-safe stateless completion provider function.
     */
    lsp::requests::TextDocument_Completion::Result ProcessCompletion(
        const lsp::requests::TextDocument_Completion::Params &req,
        const Document &doc,
        const analysis::SymbolTable &table,
        const asIScriptEngine *engine);

} // namespace angel_lsp::features::completion

namespace angel_lsp::features
{
    using completion::ProcessCompletion;
}
