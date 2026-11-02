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
} // namespace angel_lsp::features
