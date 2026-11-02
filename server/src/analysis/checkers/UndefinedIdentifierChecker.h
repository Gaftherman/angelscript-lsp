#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include <ankerl/unordered_dense.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

using MixinRanges = std::vector<std::pair<uint32_t, uint32_t>>;

/**
 * @brief Diagnostics context for reporting undefined identifiers and suggesting typo corrections.
 */
struct UndefinedIdentifierContext
{
    const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, std::equal_to<>>& knownGlobalNames;
    const MixinRanges& mixinRanges;
    const std::vector<std::string>& usingNamespaces;
    DiagnosticContext& ctx;
};

/**
 * @brief Checks for references to undefined or undeclared identifiers.
 */
struct UndefinedIdentifierChecker
{
    static constexpr std::string_view Name = "UndefinedIdentifierChecker";

    /**
     * @brief Determines whether undefined identifier checking is enabled for this request.
     * @param[in] request Semantic analysis request.
     * @return True if scopeRoot is present.
     */
    static bool IsEnabled(const SemanticAnalysisRequest& request)
    {
        return request.scopeRoot != nullptr;
    }

    /**
     * @brief Executes undefined identifier checks over the document scope hierarchy.
     * @param[in] ast AST check context containing root node, source view, and scope tree.
     * @param[in,out] ctx Diagnostic context accumulating emitted findings.
     */
    static void Run(const AstCheckContext& ast, DiagnosticContext& ctx);

    /**
     * @brief Recursively checks every LocalReference in scope and descendants against local scopes and global names.
     * @param[in] scope Root scope to inspect.
     * @param[in] knownGlobalNames Set of recognized global symbol names.
     * @param[in,out] ctx Diagnostic context accumulating emitted findings.
     */
    static void
    CheckUndefinedIdentifiers(const Scope* scope,
                              const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash,
                                                                 std::equal_to<>>& knownGlobalNames,
                              DiagnosticContext& ctx);
};

} // namespace angel_lsp::analysis
