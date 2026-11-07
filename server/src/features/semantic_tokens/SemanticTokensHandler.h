#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SemanticTokenFacts.h"
#include "analysis/index/SymbolTable.h"
#include "utils/PreprocessorRegions.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Context and input parameters for a semantic tokens request.
 */
struct SemanticTokensRequest
{
    SemanticTokensRequest(std::string_view u, std::string_view sc, const analysis::SemanticTokenFacts& f,
                          const analysis::SymbolTable& st)
        : uri(u), sourceCode(sc), facts(f), symbolTable(st)
    {
    }

    std::string_view uri;
    std::string_view sourceCode;
    const analysis::SemanticTokenFacts& facts;
    const analysis::SymbolTable& symbolTable;

    /**
     * @brief Scope tree of the document, used to tell what an identifier reference refers to.
     */
    std::shared_ptr<const analysis::Scope> scopeRoot;

    /**
     * @brief Restricts the result to the tokens overlapping this range.
     */
    std::optional<lsp::Range> range;

    /**
     * @brief Line ranges removed by preprocessor inactive directives.
     */
    std::vector<angel_lsp::utils::ExcludedLineRange> excludedLineRanges;
};

/**
 * @brief Computes full semantic tokens stream using pre-extracted syntax facts and semantic resolution.
 * @param[in] request Immutable context for semantic tokenization.
 * @return SemanticTokens struct containing encoded 5-tuple integer stream.
 */
[[nodiscard]] lsp::SemanticTokens GetSemanticTokens(const SemanticTokensRequest& request);

/**
 * @brief Returns server SemanticTokensLegend with token types and modifiers.
 * @return Reference to static legend singleton.
 */
[[nodiscard]] const lsp::SemanticTokensLegend& GetSemanticTokensLegend();

/**
 * @brief Computes minimal difference edits between two semantic token integer streams.
 * @param[in] previous Token data the client already holds.
 * @param[in] current Token data freshly computed.
 * @return List of edits to transform previous stream into current stream.
 */
[[nodiscard]] std::vector<lsp::SemanticTokensEdit> ComputeSemanticTokensDelta(const std::vector<lsp::uint>& previous,
                                                                              const std::vector<lsp::uint>& current);

} // namespace angel_lsp::features
