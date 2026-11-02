#pragma once

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/diagnostics/Diagnostics.h"
#include "analysis/index/ScopeTree.h"
#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "utils/LspLogger.h"

namespace angel_lsp::analysis
{
class NodeIndex;

/**
 * @brief Orchestrates semantic analysis on symbol tables by invoking domain-specific rule modules.
 */
class SemanticAnalyzer
{
  public:
    explicit SemanticAnalyzer(angel_lsp::utils::LspLogger* logger = nullptr);
    ~SemanticAnalyzer() = default;

    /**
     * @brief Runs semantic analysis for a document in the request context.
     * @param request Analysis request bundle containing document, symbol table, and settings.
     * @return Vector of emitted LSP diagnostics.
     */
    std::vector<Diagnostic> Analyze(const SemanticAnalysisRequest& request) const;

  private:
    angel_lsp::utils::LspLogger* m_logger;

    /**
     * @brief Dispatches every declaration in the analysed document to its rule module.
     */
    void CheckDeclarationRules(const SymbolTable& symbolTable, DiagnosticContext& ctx) const;

    /**
     * @brief Dumps debug symbol information if trace logging is enabled.
     * @param[in] request The semantic analysis request containing the symbol table.
     */
    void LogSymbolDump(const SemanticAnalysisRequest& request) const;

    /**
     * @brief Executes scope-level rules such as unused variables and undefined identifiers.
     * @param[in] request The semantic analysis request.
     * @param[in,out] ctx Diagnostic context accumulating emitted diagnostics.
     */
    double RunScopeRules(const SemanticAnalysisRequest& request, DiagnosticContext& ctx) const;

    /**
     * @brief Executes statement and control flow rules (breaks, returns, loops).
     * @param[in] request The semantic analysis request.
     * @param[in] indexPtr Optional prebuilt node index.
     * @param[in,out] ctx Diagnostic context accumulating emitted diagnostics.
     */
    void RunStatementAndControlFlowRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                         DiagnosticContext& ctx) const;

    /**
     * @brief Executes expression-level rules (calls, member expressions, assignments).
     * @param[in] request The semantic analysis request.
     * @param[in] indexPtr Optional prebuilt node index.
     * @param[in,out] ctx Diagnostic context accumulating emitted diagnostics.
     */
    void RunExpressionRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                            DiagnosticContext& ctx) const;

    /**
     * @brief Timing metrics for AST-level rule evaluations.
     */
    struct TimedAstBreakdown
    {
        double stmtMs = 0.0;     ///< Elapsed milliseconds for statement and control flow rules.
        double typeMs = 0.0;     ///< Elapsed milliseconds for type and structural rules.
        double exprMs = 0.0;     ///< Elapsed milliseconds for expression rules.
        double typeConvMs = 0.0; ///< Elapsed milliseconds for type conversion checker.
        double unusedMs = 0.0;   ///< Elapsed milliseconds for unused symbol checker.
    };

    /**
     * @brief Executes statement, type, and expression rules, recording high-resolution elapsed timings.
     * @param[in] request The semantic analysis request.
     * @param[in] indexPtr Optional prebuilt node index.
     * @param[in,out] ctx Diagnostic context accumulating emitted diagnostics.
     * @return TimedAstBreakdown containing execution times in milliseconds.
     */
    TimedAstBreakdown RunTimedAstRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                       DiagnosticContext& ctx) const;

    /**
     * @brief Executes type and structural rules (enums, interfaces, classes).
     * @param[in] request The semantic analysis request.
     * @param[in] indexPtr Optional prebuilt node index.
     * @param[in,out] ctx Diagnostic context accumulating emitted diagnostics.
     */
    void RunTypeAndStructureRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                  DiagnosticContext& ctx) const;
};

/**
 * @brief RAII guard managing enter/exit scope stack boundaries during semantic analysis passes.
 */
struct AnalysisContextGuards
{
    const SemanticAnalysisRequest& req;
    ExpressionTypeCache* prevCache;
    const NodeIndex* prevIndex;
    ExpressionTypeCache localCache;
    std::unique_ptr<NodeIndex> localNodeIndex;

    explicit AnalysisContextGuards(const SemanticAnalysisRequest& request)
        : req(request), prevCache(request.exprCache), prevIndex(request.nodeIndex)
    {
        if (!req.exprCache)
        {
            const size_t typeCapacity = req.sourceCode.size() > 10000 ? (req.sourceCode.size() / 32) : 512;
            localCache.types.reserve(typeCapacity);
            localCache.hierarchies.reserve(256);
            req.exprCache = &localCache;
        }
        if (!req.nodeIndex && req.tree)
        {
            localNodeIndex = std::make_unique<NodeIndex>(ts_tree_root_node(req.tree), nullptr, req.traversalBudget);
            req.nodeIndex = localNodeIndex.get();
        }
    }

    ~AnalysisContextGuards()
    {
        req.exprCache = prevCache;
        req.nodeIndex = prevIndex;
    }
};

} // namespace angel_lsp::analysis
