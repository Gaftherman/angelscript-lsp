#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
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

        /**
         * @brief Scope tree of the document, used to tell what an identifier reference refers to.
         *
         * The highlights query is purely syntactic and cannot distinguish a parameter read from a
         * field read from a local read - all three are just an identifier in an expression. Given
         * the scope tree, each reference is resolved to its declaration and reported as whatever
         * that declaration is. Optional: left null, references fall back to plain "variable".
         */
        std::shared_ptr<const analysis::Scope> scopeRoot;
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
