#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for a semantic tokens request.
     */
    struct SemanticTokensRequest
    {
        SemanticTokensRequest(const std::string &u,
                              const std::string &sc,
                              TSTree *t,
                              const analysis::SymbolTable &st,
                              std::shared_ptr<const analysis::Scope> sr = nullptr,
                              std::optional<lsp::Range> r = std::nullopt)
            : uri(u), sourceCode(sc), tree(t), symbolTable(st), scopeRoot(std::move(sr)), range(std::move(r)) {}

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

        /**
         * @brief Restricts the result to the tokens overlapping this range. Empty means the whole
         *        document, which is what textDocument/semanticTokens/full asks for.
         *
         * The token stream is delta-encoded against its own predecessor, so a client cannot slice
         * a full result itself - the first token of any slice would still be encoded relative to
         * whatever preceded it. Narrowing has to happen before encoding, which is why it is a
         * request field rather than post-processing.
         */
        std::optional<lsp::Range> range;
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

    /**
     * @brief Describes how to turn one token stream into another.
     *
     * A whole-document re-tokenisation is cheap here, but sending it is not: the payload is five
     * integers per token, and a typing session re-sends all of them on every keystroke. Since an
     * edit almost always leaves the run before it and the run after it untouched, the difference
     * is expressible as a single splice.
     *
     * @param previous Token data the client already has.
     * @param current Token data just computed.
     * @return Edits to apply, in the order given. Empty when the two streams are identical.
     */
    std::vector<lsp::SemanticTokensEdit> ComputeSemanticTokensDelta(const std::vector<lsp::uint> &previous,
                                                                    const std::vector<lsp::uint> &current);
}
