#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <string>
#include <optional>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a hover request.
     */
    struct HoverRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
        const analysis::ScopeIndex &scopeIndex;
        lsp::Position position;
    };

    /**
     * @brief Computes hover tooltip information for symbol under cursor.
     * @param request Immutable context for hover computation.
     * @return Optional Hover object, nullopt if no symbol found.
     */
    std::optional<lsp::Hover> GetHover(const HoverRequest &request);

    /**
     * @brief Extracts and formats Doxygen/doc comments preceding a declaration.
     * @param sourceCode Source text.
     * @param declStartLine Zero-based line where declaration begins.
     * @return Formatted markdown docstring.
     */
    std::string ExtractDocComment(const std::string &sourceCode, uint32_t declStartLine);
}
