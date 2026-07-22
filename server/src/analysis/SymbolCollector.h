/**
 * @file SymbolCollector.h
 * @brief Tree-Sitter AST symbol collector for global and local AngelScript declarations.
 * @ingroup Analysis
 */

#pragma once
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include <tree_sitter/api.h>

namespace analysis
{
    /**
     * @brief Utility class to traverse Tree-Sitter AST and populate SymbolTable.
     * @note Thread-safe stateless symbol collection passes.
     */
    class SymbolCollector
    {
    public:
        /**
         * @brief Collects global symbols (functions, classes, namespaces, globals) from document.
         *
         * @param[in] doc The document containing the source code.
         * @param[out] table The symbol table to populate.
         * @param[in] docResolver Optional callback to resolve documents already open in memory.
         * @note Traverses top-level AST nodes and handles #include directives recursively.
         */
        static void CollectGlobals(const Document &doc, SymbolTable &table, std::function<const Document *(const std::string &)> docResolver = nullptr);

        /**
         * @brief Collects local symbols and parameters for all functions/methods in document.
         *
         * @param[in] doc The document containing the source code.
         * @param[out] table The symbol table to populate.
         * @note Traverses function bodies and parameter lists.
         */
        static void CollectLocals(const Document &doc, SymbolTable &table);

        /**
         * @brief Updates preprocessor defined words for filtering symbols in inactive #if blocks.
         *
         * @param[in] defines Vector of defined preprocessor flags.
         */
        static void SetDefinedWords(const std::vector<std::string> &defines);

        /**
         * @brief Collects local symbols (local variables) from an AST node subtree.
         *
         * @param[in] node The AST node to traverse from.
         * @param[in] doc The document containing the source code.
         * @param[out] table The symbol table to populate.
         * @param[in] currentScope The parent scope symbol pointer, if any.
         */
        static void TraverseLocals(TSNode node, const Document &doc, SymbolTable &table, Symbol *currentScope = nullptr);

        /**
         * @brief Utility to register parameter nodes as local symbols.
         *
         * @param[in] paramListNode The AST node representing parameter list.
         * @param[in] doc The document containing the source code.
         * @param[out] table The symbol table to populate.
         * @param[in] parentFuncName Parent function name string.
         */
        static void RegisterParamsAsLocals(TSNode paramListNode, const Document &doc, SymbolTable &table, const std::string &parentFuncName = "");

        /**
         * @brief Extracts text from a Tree-Sitter syntax node.
         *
         * @param[in] node The AST node.
         * @param[in] doc The document containing the source code text.
         * @return std::string Extracted text substring.
         */
        static std::string GetNodeText(TSNode node, const Document &doc);

        /**
         * @brief Computes LSP Range for a Tree-Sitter AST node.
         *
         * @param[in] node The AST node.
         * @param[in] doc The document containing source text.
         * @return lsp::Range LSP 0-indexed position range.
         */
        static lsp::Range GetRange(TSNode node, const Document &doc);

        /**
         * @brief Extracts relative include path from a #include directive text.
         *
         * @param[in] text The raw include text string.
         * @return std::string Cleaned include path string.
         */
        static std::string ExtractIncludePath(std::string_view text);

        /**
         * @brief Resolves a relative include path against a base file URI into an absolute URI.
         *
         * @param[in] baseUri Base document URI.
         * @param[in] relPath Relative include path.
         * @param[in] searchDirs Vector of search directory paths.
         * @return std::string Absolute resolved URI.
         */
        static std::string ResolveIncludeUri(std::string_view baseUri, std::string_view relPath, const std::vector<std::string> &searchDirs = {});

        /**
         * @brief Recursive helper to traverse AST for global symbol declarations.
         *
         * @param[in] node The AST node to traverse.
         * @param[in] doc The document containing source text.
         * @param[out] table The symbol table to populate.
         * @param[in] parentScope Parent scope symbol pointer.
         */
        static void TraverseGlobals(TSNode node, const Document &doc, SymbolTable &table, Symbol *parentScope);
    };
}
