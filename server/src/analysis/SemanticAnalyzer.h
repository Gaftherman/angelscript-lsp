#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalysisRequest.h"
#include <ankerl/unordered_dense.h>
#include <string>
#include <vector>

// Forward-declared so semantic analysis stays decoupled from <lsp/messages.h> pulled by LspLogger.
namespace angel_lsp::utils { class LspLogger; }

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

        /**
         * @brief Recursively checks every LocalReference in scope and its descendants against the
         *        local scope chain (ResolveInScope) and knownGlobalNames, emitting
         *        as-warn-undeclared-identifier for anything that resolves to neither. Skips
         *        references flagged isMemberAccess (see ScopeTree.h) - those need type-aware
         *        member lookup this pass doesn't do, and would otherwise flood every "obj.member"
         *        access in the document with false positives.
         */
        void CheckUndefinedIdentifiers(const Scope *scope, const ankerl::unordered_dense::set<std::string> &knownGlobalNames, DiagnosticContext &ctx, int depth = 0) const;

        /**
         * @brief Pass 1 of the unused-variable check: recursively resolves every non-member-access
         *        LocalReference in scope and its descendants via ResolveInScope (starting from the
         *        reference's own containing scope, so shadowing is respected the same way
         *        ResolveInScope already defines it), and records the resolved LocalDefinition as
         *        used - unless the reference IS that definition's own occurrence (every definition's
         *        name is itself also captured as a bare @local.reference, so without this exclusion
         *        every variable would trivially look used by its own declaration).
         */
        void CollectUsedDefinitions(const Scope *scope, ankerl::unordered_dense::set<const LocalDefinition *> &used, int depth = 0) const;

        /**
         * @brief Pass 2 of the unused-variable check: recursively emits as-warn-unused-variable for
         *        every LocalDefinitionKind::Variable definition not present in used. Deliberately
         *        excludes Parameter (unused parameters are common and legitimate - interface
         *        overrides, callback signatures) and only answers "is this name referenced anywhere,"
         *        not "is this value ever read after being assigned" (LocalReference doesn't
         *        distinguish reads from writes).
         */
        void CheckUnusedVariables(const Scope *scope, const ankerl::unordered_dense::set<const LocalDefinition *> &used, DiagnosticContext &ctx, int depth = 0) const;

        /**
         * @brief Emits as-err-null-non-handle for every module-scope global or class-body field in
         *        the current document whose declared type is a VM-intrinsic numeric/bool primitive
         *        (not a handle - no trailing '@' - and confirmed by corpus audit to exclude class/
         *        object types, since those can be app-registered with a converting constructor or
         *        opAssign(int) that legitimately accepts null - see IsUnconditionallyNonNullablePrimitive
         *        in SemanticAnalyzer.cpp) whose initializer is a top-level 'null'
         *        (VariableSignature::hasNullInitializer, computed via IsNullInitializer - see
         *        TypeExtraction.h). Only covers symbols SymbolCollector already gives a
         *        VariableSignature to (module/class scope); function-body locals are handled
         *        separately, since they never get a SymbolTable entry (see LocalScopeCollector.h).
         */
        void CheckNullAssignedToNonHandle(const SymbolTable &symbolTable, DiagnosticContext &ctx) const;

        /**
         * @brief Dispatches every declaration in the analysed document to its rule module.
         *
         * The rule modules under analysis/rules/ each own one declaration kind and read only the
         * SymbolTable, so this is a single pass over it. Declarations belonging to other files of
         * the same #include module are skipped: they are indexed so this document's references
         * resolve, not so they can be diagnosed here.
         */
        void CheckDeclarationRules(const SymbolTable &symbolTable, DiagnosticContext &ctx) const;

        /**
         * @brief The rules a host's SetEngineProperty calls decide, walked in one pass.
         *
         * AngelScript is a family of dialects rather than one language, and several of these are
         * undecidable from script text alone - which is why they sat unimplemented. Each reads the
         * matching accessor on SemanticAnalysisRequest, which answers the ENGINE's default when the
         * host supplied no configuration, so a workspace that says nothing is judged the way its
         * engine will judge it.
         *
         * Covers: a plain "..." spanning lines (asEP_ALLOW_MULTILINE_STRINGS - a """heredoc""" is
         * left alone under every setting), `foreach` where the host disabled it
         * (asEP_FOREACH_SUPPORT, which is ON by default), and a hole in an initializer list
         * (asEP_DISALLOW_EMPTY_LIST_ELEMENTS).
         */
        void CheckEngineDialectRules(TSNode node, DiagnosticContext &ctx, int depth = 0) const;

        /**
         * @brief Same rule as CheckNullAssignedToNonHandle, applied to function-body locals -
         *        ScopeTree::LocalDefinition entries of kind Variable in an isFunctionScope-nested
         *        scope (same ancestor-walk pattern as CheckUnusedVariables), using the
         *        isHandleType/hasNullInitializer/typeKind fields LocalScopeCollector populates for
         *        them. Locals never get a SymbolTable entry (see LocalScopeCollector.h), so this is
         *        a separate pass rather than an extension of CheckNullAssignedToNonHandle.
         */
        void CheckNullAssignedToNonHandleInScope(const Scope *scope, DiagnosticContext &ctx, int depth = 0) const;

        /**
         * @brief Checks local variable declarations (e.g. disallowing 'void' type local variables).
         */
        void CheckLocalVariableDeclarations(const Scope *scope, DiagnosticContext &ctx, int depth = 0) const;
    };
}
