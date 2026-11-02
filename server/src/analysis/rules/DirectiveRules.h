#pragma once

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"

#include <vector>

namespace angel_lsp::analysis
{
} // namespace angel_lsp::analysis

namespace angel_lsp::analysis::rules
{

/**
 * @brief Checks preprocessor directive validity and module context warnings.
 * @param[in] request Semantic analysis request containing unsupported directives and module context.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckDirectivesAndModules(const SemanticAnalysisRequest& request, DiagnosticContext& ctx);

/**
 * @brief Checks whether metadata syntax is used when metadata support is disabled.
 * @param[in] request Semantic analysis request.
 * @param[in] indexPtr NodeIndex of the document.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckMetadataSupport(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr, DiagnosticContext& ctx);

/**
 * @brief Filters diagnostics by excluded line ranges, inline comments, and syntax recovery.
 * @param[in,out] diagnostics Vector of emitted diagnostics to filter in-place.
 * @param[in] request Semantic analysis request.
 * @param[in] indexPtr NodeIndex of the document.
 */
void FilterSuppressedDiagnostics(std::vector<Diagnostic>& diagnostics, const SemanticAnalysisRequest& request,
                                 const NodeIndex* indexPtr);

} // namespace angel_lsp::analysis::rules
