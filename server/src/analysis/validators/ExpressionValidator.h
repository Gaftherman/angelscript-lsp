/**
 * @file ExpressionValidator.h
 * @brief Semantic validator for AngelScript expressions, casts, assignments, and operators.
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
     * @brief Validator for expression, cast, and assignment AST nodes.
     * @note Thread-safe stateless validator component.
     */
    class ExpressionValidator
    {
    public:
        /**
         * @brief Validates a binary expression node (numeric operands, logical operands, handle comparisons).
         *
         * @param[in] node The Tree-Sitter AST node for binary expression.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateExpression(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates an explicit cast node (cast<T>(expr)).
         *
         * @param[in] node The Tree-Sitter AST node for cast.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateCast(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates an assignment node (reassignment to const variables/parameters).
         *
         * @param[in] node The Tree-Sitter AST node for assignment.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable Global symbol table.
         * @param[in] localTable Local symbol table.
         * @param[in] locale User locale for diagnostic message formatting.
         * @return std::vector<lsp::Diagnostic> List of emitted diagnostics.
         */
        static std::vector<lsp::Diagnostic> ValidateAssign(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates member access expressions for private member accessibility.
         */
        static std::vector<lsp::Diagnostic> ValidateMemberAccess(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates function call arguments for reference parameter L-value requirements (&out, &inout).
         */
        static std::vector<lsp::Diagnostic> ValidateCallArguments(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates increment/decrement expressions (++ / -- on const or literal operands).
         */
        static std::vector<lsp::Diagnostic> ValidateIncrementDecrement(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates lambda expressions against target funcdef handles.
         */
        static std::vector<lsp::Diagnostic> ValidateLambda(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);

        /**
         * @brief Validates ternary conditional expressions (? :) for branch type compatibility.
         */
        static std::vector<lsp::Diagnostic> ValidateTernary(
            TSNode node,
            const Document &doc,
            const SymbolTable &globalTable,
            const SymbolTable &localTable,
            i18n::Locale locale);
    };

} // namespace analysis::validators
