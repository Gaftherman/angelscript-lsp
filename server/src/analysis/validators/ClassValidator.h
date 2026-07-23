/**
 * @file ClassValidator.h
 * @brief Semantic validator for AngelScript classes, interfaces, mixins, and virtual properties.
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
     * @brief Validator for class, interface, and virtprop AST nodes.
     * @note Thread-safe stateless validator component.
     */
    class ClassValidator
    {
    public:
        /**
         * @brief Validates a class AST node (inheritance, final inheritance, interface implementation, override/final checks).
         *
         * @param[in] node The Tree-Sitter AST node for class declaration.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateClass(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates an interface AST node.
         *
         * @param[in] node The Tree-Sitter AST node for interface declaration.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateInterface(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates a virtual property AST node (accessor return/parameter type checks).
         *
         * @param[in] node The Tree-Sitter AST node for virtprop.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateVirtProp(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates instantiation of types (prohibiting mixin class instantiation).
         */
        static std::vector<lsp::Diagnostic> ValidateInstantiation(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates operator overload method signatures (opAdd, opCmp, opEquals, opIndex, etc.).
         *
         * @param[in] funcNode The Tree-Sitter AST node for a class method.
         * @param[in] doc The document containing source text.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateOperatorOverloads(
            TSNode funcNode,
            const Document &doc,
            i18n::Locale locale);
    };

} // namespace analysis::validators
