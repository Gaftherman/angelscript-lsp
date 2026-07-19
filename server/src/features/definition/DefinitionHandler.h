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
         * @brief Processes an LSP Goto Definition request.
         *
         * @param req The definition request parameters (document URI and cursor position).
         * @param doc The parsed Document containing the Tree-Sitter AST and source code.
         * @param table The SymbolTable containing resolved symbols.
         * @param engine Optional pointer to the AngelScript engine.
         * @return lsp::requests::TextDocument_Definition::Result The target location(s).
         */
        lsp::requests::TextDocument_Definition::Result ProcessDefinition(
            const lsp::requests::TextDocument_Definition::Params &req,
            const Document &doc,
            const analysis::SymbolTable &table,
            const asIScriptEngine *engine);

    }
}
