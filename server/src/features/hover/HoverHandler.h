/**
 * @file HoverHandler.h
 * @brief Handler implementation for Language Server Protocol (LSP) textDocument/hover requests.
 * @ingroup Features
 */

#pragma once

#include <lsp/messages.h>
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include "analysis/DiagnosticCache.h"
#include "i18n/LspStrings.h"

namespace angel_lsp::features::hover
{
    /**
     * @brief Processes an LSP Hover request to provide Markdown tooltips and symbol documentation.
     *
     * @param[out] result The output result object to populate with hover contents.
     * @param[in] req The hover request parameters (document URI and cursor position).
     * @param[in] doc The parsed Document containing Tree-Sitter AST and source text.
     * @param[in] table The SymbolTable containing resolved symbols.
     * @param[in] diagCache Optional DiagnosticCache pointer to include inline diagnostic messages.
     * @param[in] locale Target localization locale.
     * @note Thread-safe stateless hover provider function.
     */
    void ProcessHover(lsp::requests::TextDocument_Hover::Result &result,
                      const lsp::requests::TextDocument_Hover::Params &req,
                      const Document &doc,
                      const analysis::SymbolTable &table,
                      const analysis::DiagnosticCache *diagCache = nullptr,
                      i18n::Locale locale = i18n::Locale::EN);

} // namespace angel_lsp::features::hover

namespace angel_lsp::features
{
    using hover::ProcessHover;
}
