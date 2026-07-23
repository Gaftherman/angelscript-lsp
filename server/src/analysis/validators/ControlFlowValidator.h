/**
 * @file ControlFlowValidator.h
 * @brief Semantic validator for AngelScript control flow statements (return, break, continue, switch, foreach).
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
     * @brief Validator for control flow AST nodes.
     * @note Thread-safe stateless validator component.
     */
    class ControlFlowValidator
    {
    public:
        /**
         * @brief Validates break and continue statements within function bodies.
         *
         * @param[in] funcNode The Tree-Sitter AST node for function or funcdef.
         * @param[in] doc The document containing source text.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateControlFlow(
            TSNode funcNode,
            const Document &doc,
            i18n::Locale locale);

        /**
         * @brief Validates a switch statement AST node (control expression type, case match, case duplicates).
         *
         * @param[in] switchNode The Tree-Sitter AST node for switch.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateSwitch(
            TSNode switchNode,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates a foreach statement AST node.
         *
         * @param[in] foreachNode The Tree-Sitter AST node for foreach.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateForeach(
            TSNode foreachNode,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);
    };

} // namespace analysis::validators
