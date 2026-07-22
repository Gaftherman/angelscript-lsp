#pragma once
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include <tree_sitter/api.h>

namespace analysis
{
    /**
     * @brief Utility class to traverse the AST and populate a SymbolTable.
     */
    class SymbolCollector
    {
    public:
        /**
         * @brief Collects global symbols (functions, classes, namespaces, globals) and adds them to the global table.
         *
         * @param doc The document containing the source code.
         * @param table The symbol table to populate.
         * @param docResolver Optional callback to resolve documents already open in memory.
         */
        static void CollectGlobals(const Document &doc, SymbolTable &table, std::function<const Document *(const std::string &)> docResolver = nullptr);

        /**
         * @brief Collects local symbols and parameters for all functions/methods in the document.
         *
         * @param doc The document containing the source code.
         * @param table The symbol table to populate.
         */
        static void CollectLocals(const Document &doc, SymbolTable &table);

        /**
         * @brief Updates the preprocessor defined words used for filtering symbols in inactive #if blocks.
         */
        static void SetDefinedWords(const std::vector<std::string> &defines);

        /**
         * @brief Collects local symbols (local variables inside function bodies) and adds them to the local table.
         *
         * @param node The AST node to traverse from.
         * @param doc The document containing the source code.
         * @param table The symbol table to populate.
         * @param currentScope The current scope symbol, if any.
         */
        static void TraverseLocals(TSNode node, const Document &doc, SymbolTable &table, Symbol *currentScope = nullptr);

        /**
         * @brief Utility to register parameters as local variables.
         *
         * @param paramListNode The AST node representing the parameter list.
         * @param doc The document containing the source code.
         * @param table The symbol table to populate.
         */
        static void RegisterParamsAsLocals(TSNode paramListNode, const Document &doc, SymbolTable &table, const std::string &parentFuncName = "");

        /**
         * @brief Utility to extract text from an AST node.
         *
         * @param node The AST node.
         * @param doc The document containing the source code.
         * @return The extracted text.
         */
        static std::string GetNodeText(TSNode node, const Document &doc);

        /**
         * @brief Factory for symbol ranges based on AST nodes.
         *
         * @param node The AST node.
         * @param doc The document containing the source code.
         * @return The range of the node.
         */
        static lsp::Range GetRange(TSNode node, const Document &doc);

        /**
         * @brief Extracts the relative include path from a #include directive text.
         */
        static std::string ExtractIncludePath(std::string_view text);

        /**
         * @brief Resolves a relative include path against a base file URI into an absolute file URI.
         * Automatically checks for auto-appended extensions (e.g. .as) and search directories.
         */
        static std::string ResolveIncludeUri(std::string_view baseUri, std::string_view relPath, const std::vector<std::string> &searchDirs = {});

        /**
         * @brief Recursive helper to traverse AST for globals.
         *
         * @param node The AST node to traverse.
         * @param doc The document containing the source code.
         * @param table The symbol table to populate.
         * @param parentScope The parent scope symbol.
         */
        static void TraverseGlobals(TSNode node, const Document &doc, SymbolTable &table, Symbol *parentScope);
    };
}
