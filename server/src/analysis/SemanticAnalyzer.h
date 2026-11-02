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
     * @brief Recursively checks every LocalReference in scope and its descendants against the
     *        local scope chain (ResolveInScope) and knownGlobalNames, emitting
     *        as-warn-undeclared-identifier for anything that resolves to neither. Skips
     *        references flagged isMemberAccess (see ScopeTree.h) - those need type-aware
     *        member lookup this pass doesn't do, and would otherwise flood every "obj.member"
     *        access in the document with false positives.
     */
    void CheckUndefinedIdentifiers(const Scope* scope,
                                   const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash,
                                                                      std::equal_to<>>& knownGlobalNames,
                                   DiagnosticContext& ctx) const;

    /**
     * @brief Emits as-err-null-non-handle for every module-scope global or class-body field in
     *        the current document whose declared type is a non-handle primitive initialized to null.
     */
    void CheckNullAssignedToNonHandle(const SymbolTable& symbolTable, DiagnosticContext& ctx) const;

    /**
     * @brief Dispatches every declaration in the analysed document to its rule module.
     */
    void CheckDeclarationRules(const SymbolTable& symbolTable, DiagnosticContext& ctx) const;

    /**
     * @brief Fast single-pass engine dialect check using NodeIndex.
     */
    void CheckEngineDialectRules(const NodeIndex& nodeIndex, DiagnosticContext& ctx) const;

    /**
     * @brief Same rule as CheckNullAssignedToNonHandle, applied to function-body locals -
     *        ScopeTree::LocalDefinition entries of kind Variable in an isFunctionScope-nested
     *        scope (same ancestor-walk pattern as CheckUnusedVariables), using the
     *        isHandleType/hasNullInitializer/typeKind fields LocalScopeCollector populates for
     *        them. Locals never get a SymbolTable entry (see LocalScopeCollector.h), so this is
     *        a separate pass rather than an extension of CheckNullAssignedToNonHandle.
     */
    void CheckNullAssignedToNonHandleInScope(const Scope* scope, DiagnosticContext& ctx) const;

    /**
     * @brief Checks local variable declarations (e.g. disallowing 'void' type local variables).
     */
    void CheckLocalVariableDeclarations(const Scope* scope, DiagnosticContext& ctx) const;

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

    /**
     * @brief Checks preprocessor directives and module boundaries.
     * @param[in] request The semantic analysis request.
     * @param[in,out] ctx Diagnostic context accumulating emitted diagnostics.
     */
    void CheckDirectivesAndModules(const SemanticAnalysisRequest& request, DiagnosticContext& ctx) const;

    /**
     * @brief Validates names declared within a local scope (e.g., duplicates, reserved keywords).
     * @param[in] scope The current lexical scope being inspected.
     * @param[in,out] ctx Diagnostic context accumulating emitted diagnostics.
     */
    void CheckLocalNames(const Scope* scope, DiagnosticContext& ctx) const;

    /**
     * @brief Validates types used by local variables within a scope.
     * @param[in] scope The current lexical scope being inspected.
     * @param[in,out] ctx Diagnostic context accumulating emitted diagnostics.
     */
    void CheckLocalTypes(const Scope* scope, DiagnosticContext& ctx) const;
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
} // namespace angel_lsp::analysis
