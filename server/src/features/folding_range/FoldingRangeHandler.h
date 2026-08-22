#pragma once

#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <optional>

namespace angel_lsp::features
{
    /**
     * @brief Context and immutable input parameters for a Folding Range request.
     */
    struct FoldingRangeRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
    };

    /**
     * @brief List of folding ranges extracted for document folding.
     */
    using FoldingRangeResult = std::vector<lsp::FoldingRange>;

    /**
     * @brief Extracts folding regions (classes, interfaces, namespaces, functions,
     *        control blocks, comments, and preprocessor directives) from the AST.
     * @param request Immutable context for folding range computation.
     * @return Optional vector of FoldingRange items; nullopt if document cannot be parsed.
     */
    std::optional<FoldingRangeResult> GetFoldingRanges(const FoldingRangeRequest &request);
}
