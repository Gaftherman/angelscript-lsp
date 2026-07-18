#pragma once
#include <lsp/messages.h>
#include "document/Document.h"

#include "analysis/SymbolTable.h"
#include "analysis/DiagnosticCache.h"
#include "i18n/LspStrings.h"

class asIScriptEngine;

namespace angel_lsp
{
namespace features
{

/**
 * @brief Processes an LSP Hover request to provide rich tooltips.
 * 
 * @param result The output result to populate with hover content.
 * @param req The hover request parameters (document URI and cursor position).
 * @param doc The parsed Document containing the Tree-Sitter AST and source code.
 * @param table The SymbolTable containing resolved symbols.
 * @param diagCache Optional DiagnosticCache to show errors/warnings in hover.
 * @param locale The locale to use for translated strings in the hover.
 * @param engine Optional pointer to the AngelScript engine.
 */
void ProcessHover(lsp::requests::TextDocument_Hover::Result& result,
                  const lsp::requests::TextDocument_Hover::Params& req,
                  const Document& doc,
                  const analysis::SymbolTable& table,
                  const analysis::DiagnosticCache* diagCache,
                  i18n::Locale locale,
                  const asIScriptEngine* engine);

}
}
