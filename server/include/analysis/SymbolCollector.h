#pragma once
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include <tree_sitter/api.h>

namespace analysis
{
    class SymbolCollector
    {
    public:
        // Collects global symbols (functions, classes, namespaces, globals) and adds them to the global table.
        // It should traverse the AST starting from the root node.
        static void CollectGlobals(const Document& doc, SymbolTable& table);

        // Collects local symbols (local variables inside function bodies) and adds them to the local table.
        // Traverse the AST starting from a specific block node.
        static void TraverseLocals(TSNode node, const Document& doc, SymbolTable& table, Symbol* currentScope = nullptr);

        // Utility to register parameters as local variables
        static void RegisterParamsAsLocals(TSNode paramListNode, const Document& doc, SymbolTable& table);

        // Utility to extract text from a node
        static std::string GetNodeText(TSNode node, const Document& doc);
        
        // Factory for symbol ranges
        static lsp::Range GetRange(TSNode node, const Document& doc);
        
    private:
        // Recursive helper to traverse AST for globals
        static void TraverseGlobals(TSNode node, const Document& doc, SymbolTable& table, Symbol* parentScope);
    };
}
