#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "utils/LspLogger.h"
#include <vector>

namespace angel_lsp::analysis
{
    /**
     * @brief Orchestrates semantic analysis on symbol tables by invoking domain-specific rule modules.
     */
    class SemanticAnalyzer
    {
    public:
        explicit SemanticAnalyzer(angel_lsp::utils::LspLogger *logger = nullptr);
        ~SemanticAnalyzer() = default;

        /**
         * @brief Runs semantic analysis for a document in the request context.
         * @param request Input request containing symbol table and options.
         * @return Vector of emitted LSP diagnostics.
         */
        std::vector<Diagnostic> Analyze(const SemanticAnalysisRequest &request) const;

    private:
        angel_lsp::utils::LspLogger *m_logger;
    };
}
