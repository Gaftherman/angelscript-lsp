#pragma once

#include "analysis/SymbolTable.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <string>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a semantic tokens request.
     */
    struct SemanticTokensRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
    };

    /**
     * @brief Computes full semantic tokens stream using HIGHLIGHTS_QUERY.
     * @param request Immutable context for semantic tokenization.
     * @return SemanticTokens struct containing encoded 5-tuple integer stream.
     */
    lsp::SemanticTokens GetSemanticTokens(const SemanticTokensRequest &request);

    /**
     * @brief Returns server SemanticTokensLegend with token types and modifiers.
     */
    const lsp::SemanticTokensLegend &GetSemanticTokensLegend();
}
