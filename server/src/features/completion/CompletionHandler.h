#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

#include "analysis/SymbolTable.h"

class asIScriptEngine;

namespace angel_lsp
{
    namespace features
    {

        /**
         * @brief Processes an LSP Completion request to provide auto-completion suggestions.
         *
         * @param req The completion request parameters (including document URI and cursor position).
         * @param doc The parsed Document containing the Tree-Sitter AST and source code.
         * @param table The SymbolTable containing resolved symbols for the document.
         * @param engine Optional pointer to the AngelScript engine for predefined types.
         * @return lsp::requests::TextDocument_Completion::Result The list of completion items.
         */
        lsp::requests::TextDocument_Completion::Result ProcessCompletion(
            const lsp::requests::TextDocument_Completion::Params &req,
            const Document &doc,
            const analysis::SymbolTable &table,
            const asIScriptEngine *engine);

    }
}
