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
         * @brief Processes an LSP Signature Help request to show function signatures and parameters.
         *
         * @param req The signature help request parameters.
         * @param doc The parsed Document containing the Tree-Sitter AST and source code.
         * @param table The SymbolTable containing resolved symbols.
         * @param engine Optional pointer to the AngelScript engine.
         * @return lsp::requests::TextDocument_SignatureHelp::Result The signature information.
         */
        lsp::requests::TextDocument_SignatureHelp::Result ProcessSignatureHelp(
            const lsp::requests::TextDocument_SignatureHelp::Params &req,
            const Document &doc,
            const analysis::SymbolTable &table,
            const asIScriptEngine *engine);

    }
}
