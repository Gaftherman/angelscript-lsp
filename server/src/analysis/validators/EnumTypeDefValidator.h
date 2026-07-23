/**
 * @file EnumTypeDefValidator.h
 * @brief Semantic validator for AngelScript 'enum' and 'typedef' declarations.
 * @ingroup Analysis
 */

#pragma once

#include <vector>
#include <tree_sitter/api.h>
#include <lsp/types.h>
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include "analysis/TypeEvaluator.h"
#include "i18n/LspStrings.h"

namespace analysis::validators
{
    /**
     * @brief Validator for 'enum' and 'typedef' AST nodes.
     * @note Thread-safe stateless validator component.
     */
    class EnumTypeDefValidator
    {
    public:
        /**
         * @brief Validates an 'enum' AST node for duplicate enum names, duplicate enumerators, and non-integer initializers.
         *
         * @param[in] node The Tree-Sitter AST node for enum declaration.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateEnum(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates a 'typedef' AST node for primitive target type validity and alias name collisions.
         *
         * @param[in] node The Tree-Sitter AST node for typedef declaration.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateTypedef(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);
    };

} // namespace analysis::validators
