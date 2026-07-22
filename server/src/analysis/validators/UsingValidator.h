/**
 * @file UsingValidator.h
 * @brief Semantic validator for AngelScript 'using namespace' directives.
 * @ingroup Analysis
 */

#pragma once

#include <vector>
#include <tree_sitter/api.h>
#include <lsp/types.h>
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include "i18n/LspStrings.h"

namespace analysis::validators
{
    /**
     * @brief Validator for 'using namespace' AST nodes.
     * @note Thread-safe stateless validator component.
     */
    class UsingValidator
    {
    public:
        /**
         * @brief Validates a 'using' AST node for declared namespace existence and duplicates.
         *
         * @param[in] node The Tree-Sitter AST node for using directive.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> Validate(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);
    };

} // namespace analysis::validators
