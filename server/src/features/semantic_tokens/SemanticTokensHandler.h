#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

#include "analysis/SymbolTable.h"

namespace angel_lsp
{
namespace features
{

/**
 * @brief Processes an LSP Semantic Tokens request to provide syntax highlighting.
 * 
 * @param req The semantic tokens request parameters.
 * @param doc The parsed Document containing the Tree-Sitter AST and source code.
 * @param table The SymbolTable containing resolved symbols.
 * @return lsp::requests::TextDocument_SemanticTokens_Full::Result The encoded semantic tokens.
 */
lsp::requests::TextDocument_SemanticTokens_Full::Result ProcessSemanticTokensFull(
    const lsp::requests::TextDocument_SemanticTokens_Full::Params& req,
    const Document& doc,
    const analysis::SymbolTable& table
);

}
}
