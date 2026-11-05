#pragma once

#include "analysis/index/SymbolTable.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a document symbol request.
 */
struct DocumentSymbolRequest
{
    std::string_view uri{};
    std::string_view sourceCode{};
    const analysis::SymbolTable& symbolTable;
};

/**
 * @brief Result list containing hierarchical document symbols for outline navigation.
 */
using DocumentSymbolResult = std::vector<lsp::DocumentSymbol>;

/**
 * @brief Generates hierarchical document symbols (classes, methods, fields, namespaces, functions, enums)
 *        from the indexed symbols of the given document.
 * @param[in] request Immutable context for document symbol extraction.
 * @return Optional vector of DocumentSymbol objects.
 */
[[nodiscard]] std::optional<DocumentSymbolResult> GetDocumentSymbols(const DocumentSymbolRequest& request);
} // namespace angel_lsp::features
