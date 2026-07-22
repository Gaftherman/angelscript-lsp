/**
 * @file SymbolResolver.h
 * @brief Scope resolution engine for AST identifiers, types, member accesses, and overloads.
 * @ingroup Analysis
 */

#pragma once

#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include <optional>
#include <vector>

namespace analysis
{
    /**
     * @brief Resolves identifiers in the AST to their corresponding Symbol declarations.
     * @note Thread-safe stateless symbol resolution engine.
     */
    class SymbolResolver
    {
    public:
        /**
         * @brief Resolves the symbol declaration under a specified cursor position.
         *
         * @param[in] doc The document being analyzed.
         * @param[in] table The symbol table for the document context.
         * @param[in] line Zero-indexed line number.
         * @param[in] character Zero-indexed column number.
         * @param[out] outMultipleResults Optional output vector to store all candidates (e.g. overloads).
         * @return const Symbol* Pointer to the resolved symbol, or nullptr if unresolved.
         */
        static const Symbol *ResolveAt(const Document &doc, const SymbolTable &table, uint32_t line, uint32_t character, std::vector<const Symbol *> *outMultipleResults = nullptr);

        /**
         * @brief Evaluates the type string of an expression AST node (used for type inference).
         *
         * @param[in] doc The document being analyzed.
         * @param[in] table The symbol table.
         * @param[in] exprNode The Tree-Sitter expression AST node.
         * @return std::string Inferred type string (e.g. "float", "Vector3"), or empty if unknown.
         */
        static std::string EvaluateExpressionType(const Document &doc, const SymbolTable &table, TSNode exprNode);

        /**
         * @brief Cleans a raw AngelScript type string into a pure type name identifier.
         *
         * @param[in] raw Raw type string (e.g. "const Player@ in").
         * @return std::string Cleaned type name (e.g. "Player").
         */
        static std::string CleanTypeName(std::string_view raw);

    private:

        /**
         * @brief Resolves a member access expression node (e.g. `obj.member`).
         */
        static const Symbol *ResolveMemberAccess(const Document &doc, const SymbolTable &table, TSNode node, TSNode parent, const std::string &identText, std::vector<const Symbol *> *outMultipleResults);

        /**
         * @brief Resolves a constructor or destructor call.
         */
        static const Symbol *ResolveConstructorOrDestructor(const Document &doc, const SymbolTable &table, TSNode node, TSNode parent, const std::string &identText, uint32_t line, std::vector<const Symbol *> *outMultipleResults);

        /**
         * @brief Resolves a scoped identifier node (e.g. `Namespace::Type` or `Enum::Value`).
         */
        static const Symbol *ResolveScopedIdentifier(const Document &doc, const SymbolTable &table, TSNode node, TSNode parent, const std::string &identText, std::vector<const Symbol *> &outGlobalCandidates, bool &outIsScoped);

        /**
         * @brief Resolves an implicit member access inside a class or mixin method body.
         */
        static const Symbol *ResolveImplicitMember(const Document &doc, const SymbolTable &table, TSNode node, const std::string &identText, std::vector<const Symbol *> *outMultipleResults);

        /**
         * @brief Filters and scores candidates to find the best overload or scope match.
         */
        static const Symbol *FilterAndScoreCandidates(const Document &doc, TSNode node, TSNode parent, std::string_view parentType, std::vector<const Symbol *> &globalCandidates, std::vector<const Symbol *> *outMultipleResults);
    };
}
