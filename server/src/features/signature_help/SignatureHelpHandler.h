/**
 * @file SignatureHelpHandler.h
 * @brief Handler for LSP textDocument/signatureHelp requests.
 * @ingroup Features
 */

#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

#include "analysis/SymbolTable.h"

class asIScriptEngine;

namespace angel_lsp::features::signature_help
{

    /**
     * @brief Processes an LSP Signature Help request to show active function parameters.
     *
     * @param[in] req The signature help request parameters (document URI and cursor position).
     * @param[in] doc The parsed Document containing Tree-Sitter AST and source code.
     * @param[in] table The SymbolTable containing resolved symbols.
     * @param[in] engine Optional pointer to the AngelScript engine instance.
     * @return lsp::requests::TextDocument_SignatureHelp::Result Signature information or nullopt.
     * @note Thread-safe stateless signature help provider function.
     */
    lsp::requests::TextDocument_SignatureHelp::Result ProcessSignatureHelp(
        const lsp::requests::TextDocument_SignatureHelp::Params &req,
        const Document &doc,
        const analysis::SymbolTable &table,
        const asIScriptEngine *engine);

} // namespace angel_lsp::features::signature_help

namespace angel_lsp::features
{
    using signature_help::ProcessSignatureHelp;
}
