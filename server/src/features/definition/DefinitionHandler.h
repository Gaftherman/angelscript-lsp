/**
 * @file DefinitionHandler.h
 * @brief Handlers for LSP textDocument/definition and textDocument/typeDefinition requests.
 * @ingroup Features
 */

#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

#include "analysis/SymbolTable.h"

class asIScriptEngine;

namespace angel_lsp::features::definition
{

    /**
     * @brief Processes an LSP Goto Definition request.
     *
     * @param[in] req The definition request parameters (document URI and cursor position).
     * @param[in] doc The parsed Document containing Tree-Sitter AST and source code.
     * @param[in] table The SymbolTable containing resolved symbols.
     * @param[in] engine Optional pointer to the AngelScript engine instance.
     * @return lsp::requests::TextDocument_Definition::Result Target location(s) or nullopt if unresolved.
     * @note Thread-safe stateless definition provider function.
     */
    lsp::requests::TextDocument_Definition::Result ProcessDefinition(
        const lsp::requests::TextDocument_Definition::Params &req,
        const Document &doc,
        const analysis::SymbolTable &table,
        const asIScriptEngine *engine);

    /**
     * @brief Processes an LSP Goto Type Definition request.
     *
     * @param[in] req The type definition request parameters.
     * @param[in] doc The parsed Document containing Tree-Sitter AST and source code.
     * @param[in] table The SymbolTable containing resolved symbols.
     * @param[in] engine Optional pointer to the AngelScript engine instance.
     * @return lsp::requests::TextDocument_TypeDefinition::Result Target location(s) or nullopt.
     * @note Thread-safe stateless type definition provider function.
     */
    lsp::requests::TextDocument_TypeDefinition::Result ProcessTypeDefinition(
        const lsp::requests::TextDocument_TypeDefinition::Params &req,
        const Document &doc,
        const analysis::SymbolTable &table,
        const asIScriptEngine *engine);

} // namespace angel_lsp::features::definition

namespace angel_lsp::features
{
    using definition::ProcessDefinition;
    using definition::ProcessTypeDefinition;
}
