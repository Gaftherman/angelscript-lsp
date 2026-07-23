/**
 * @file TypeEvaluator.h
 * @brief Type Inference and Type Checking engine for AngelScript AST expressions.
 * @ingroup Analysis
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include "document/Document.h"
#include "analysis/SymbolTable.h"

namespace analysis
{
    /**
     * @brief Dedicated component for inferring and checking expression types in AST.
     * @note Thread-safe stateless type evaluator.
     */
    class TypeEvaluator
    {
    public:
        /**
         * @brief Infers the evaluated type of an expression AST node.
         *
         * @param[in] exprNode The Tree-Sitter expression AST node.
         * @param[in] doc The document containing source text.
         * @param[in] globalTable The global symbol table.
         * @param[in] localTable The local symbol table.
         * @return std::optional<std::string> Inferred type string (e.g. "int", "bool", "float", "string"), or std::nullopt.
         */
        static std::optional<std::string> InferType(TSNode exprNode, const Document &doc, const SymbolTable &globalTable, const SymbolTable &localTable);

        /**
         * @brief Checks if two type names are implicitly compatible for assignment in Phase 1.
         *
         * @param[in] targetType LHS type (e.g. "int", "float").
         * @param[in] sourceType RHS type (e.g. "bool", "float").
         * @return bool True if compatible, false if type mismatch.
         */
        static bool AreTypesCompatible(std::string_view targetType, std::string_view sourceType, const SymbolTable *globalTable = nullptr);
    };

} // namespace analysis
