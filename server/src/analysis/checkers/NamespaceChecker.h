#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NamespaceUsageFacts.h"
#include "analysis/index/NodeIndex.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Contextual request bundle containing AST root, source text, node index, and optional facts.
 */
struct NamespaceCheckRequest
{
    /**
     * @brief Document source text the tree was parsed from.
     */
    std::string_view sourceCode{};

    /**
     * @brief Pre-indexed node index for fast single-pass querying.
     */
    const NodeIndex* nodeIndex = nullptr;

    /**
     * @brief Optional precomputed namespace usage facts.
     */
    const NamespaceUsageFacts* facts = nullptr;
};

/**
 * @brief Reports scope qualifiers and call targets that resolve to nothing.
 * @param[in] request Request bundle carrying source text, node index, and optional facts.
 * @param[in,out] ctx Diagnostic sink accumulating emitted findings.
 */
void CheckNamespacesAndScopes(const NamespaceCheckRequest& request, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
