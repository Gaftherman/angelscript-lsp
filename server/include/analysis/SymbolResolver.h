#pragma once

#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include <optional>
#include <vector>

namespace analysis
{
    /**
     * @brief Resolves identifiers in the AST to their corresponding Symbols.
     *
     * Handles complex scoping rules, namespaces, member access, inheritance,
     * mixins, constructors, and method overloading resolution.
     */
    class SymbolResolver
    {
    public:
        /**
         * @brief Resolves the symbol under the given cursor position.
         *
         * @param doc The document being analyzed.
         * @param table The symbol table for the document.
         * @param line The 0-indexed line of the cursor.
         * @param character The 0-indexed column of the cursor.
         * @param outMultipleResults Optional output vector to store all possible candidates (e.g. overloads).
         * @return The best matching Symbol, or nullptr if none found.
         */
        static const Symbol *ResolveAt(const Document &doc, const SymbolTable &table, uint32_t line, uint32_t character, std::vector<const Symbol *> *outMultipleResults = nullptr);

        /**
         * @brief Evaluates the actual type of an AST expression node (used for auto type inference).
         *
         * @param doc The document being analyzed.
         * @param table The symbol table.
         * @param exprNode The Tree-sitter expression node.
         * @return A string representing the inferred type (e.g., "float", "Vector3"), or empty if unknown.
         */
        static std::string EvaluateExpressionType(const Document &doc, const SymbolTable &table, TSNode exprNode);

    private:
        /**
         * @brief Cleans a raw AngelScript type string (e.g. "const Player@ in") into a pure type name ("Player").
         */
        static std::string CleanTypeName(std::string_view raw);

        /**
         * @brief Resolves a member access expression (e.g. `obj.member`).
         */
        static const Symbol *ResolveMemberAccess(const Document &doc, const SymbolTable &table, TSNode node, TSNode parent, const std::string &identText, std::vector<const Symbol *> *outMultipleResults);

        /**
         * @brief Resolves a constructor or destructor call.
         */
        static const Symbol *ResolveConstructorOrDestructor(const Document &doc, const SymbolTable &table, TSNode node, TSNode parent, const std::string &identText, uint32_t line, std::vector<const Symbol *> *outMultipleResults);

        /**
         * @brief Resolves a scoped identifier (e.g. `Namespace::Type` or `Enum::Value`).
         */
        static const Symbol *ResolveScopedIdentifier(const Document &doc, const SymbolTable &table, TSNode node, TSNode parent, const std::string &identText, std::vector<const Symbol *> &outGlobalCandidates, bool &outIsScoped);

        /**
         * @brief Resolves an implicit member access when inside a class or mixin method.
         */
        static const Symbol *ResolveImplicitMember(const Document &doc, const SymbolTable &table, TSNode node, const std::string &identText, std::vector<const Symbol *> *outMultipleResults);

        /**
         * @brief Filters and scores multiple candidates to find the best match based on context.
         */
        static const Symbol *FilterAndScoreCandidates(const Document &doc, TSNode node, TSNode parent, std::string_view parentType, std::vector<const Symbol *> &globalCandidates, std::vector<const Symbol *> *outMultipleResults);
    };
}
