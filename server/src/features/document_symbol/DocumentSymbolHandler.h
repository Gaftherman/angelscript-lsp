#pragma once

#include "analysis/index/SymbolTable.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a document symbol request.
 */
struct DocumentSymbolRequest
{
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    const analysis::SymbolTable& symbolTable;
};

/**
 * @brief Result list containing hierarchical document symbols for outline navigation.
 */
using DocumentSymbolResult = std::vector<lsp::DocumentSymbol>;

/**
 * @brief Generates hierarchical document symbols (classes, methods, fields, namespaces, functions, enums)
 *        from the Tree-Sitter AST of the given document.
 * @param request Immutable context for document symbol extraction.
 * @return Optional vector of DocumentSymbol objects; nullopt if document cannot be parsed.
 */
std::optional<DocumentSymbolResult> GetDocumentSymbols(const DocumentSymbolRequest& request);

/**
 * @brief Recursive visitor state during document symbol hierarchy construction.
 */
struct TraversalContext
{
    std::string_view sourceCode;
    bool isInsideClass = false;
    std::string_view enclosingClassName;
};

/**
 * @brief Accessor helpers for extracting return type, name, and parameter list nodes from a function declaration AST.
 */
struct FunctionComponents
{
    TSNode nameNode;
    std::string name;
    TSNode retNode;
    TSNode paramsNode;
};

/**
 * @brief Traversal engine for processing AST nodes into hierarchical LSP document symbols.
 */
struct DocumentSymbolProcessor
{
    /**
     * @brief Processes all child nodes within an AST container node.
     * @param[in] containerNode AST container node.
     * @param[in] ctx Traversal context.
     * @return Vector of child document symbols.
     */
    static std::vector<lsp::DocumentSymbol> ProcessChildren(TSNode containerNode, const TraversalContext& ctx);

    /**
     * @brief Processes an individual AST node and appends any extracted symbol to outSymbols.
     * @param[in] node AST node to process.
     * @param[in] ctx Traversal context.
     * @param[in,out] outSymbols Symbol list to append to.
     */
    static void ProcessNode(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols);
};
} // namespace angel_lsp::features
