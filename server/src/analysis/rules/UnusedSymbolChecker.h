#pragma once

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/diagnostics/Diagnostics.h"
#include "utils/StringPool.h"

namespace angel_lsp::analysis::rules
{

/**
 * @brief Checks for unused local variables, member variables, global variables, functions, and classes.
 * @param[in] request Semantic analysis request containing AST, symbol table, and engine rules.
 * @param[in,out] ctx Diagnostic context accumulating emitted warnings.
 */
void CheckUnusedSymbols(const SemanticAnalysisRequest& request, DiagnosticContext& ctx);

/**
 * @brief Checks if a function's parameter types match any configured engine callback signature.
 * @param[in] sym Candidate function symbol.
 * @param[in] rules Active rules whose engineCallbackSignatures list the expected parameter-type lists.
 * @return True if parameter types match one of the candidate signatures.
 */
[[nodiscard]] bool MatchesEngineCallbackSignature(const Symbol& sym, const config::UnusedSymbolRules& rules);

/**
 * @brief Checks if a class inherits from an engine base class listed in ignoredBaseClasses.
 * @param[in] className Name of the candidate class.
 * @param[in] symbolTable Symbol table providing type inheritance hierarchy.
 * @param[in] rules Active unused symbol rules.
 * @return True if class or any ancestor is an engine base class.
 */
[[nodiscard]] bool IsEngineEntity(std::string_view className, const SymbolTable& symbolTable,
                                  const config::UnusedSymbolRules& rules);

/**
 * @brief Checks if a method overrides a base class method or matches an engine lifecycle callback.
 * @param[in] sym Candidate member function symbol.
 * @param[in] symbolTable Symbol table providing type inheritance hierarchy.
 * @param[in] unusedRules Active unused symbol rules.
 * @return True if method is an override or lifecycle callback.
 */
[[nodiscard]] bool IsLifecycleOrOverride(const Symbol& sym, const SymbolTable& symbolTable,
                                         const config::UnusedSymbolRules& unusedRules);

/**
 * @brief Checks if a symbol is an engine-managed entry point (global hook or class lifecycle method).
 * @param[in] sym Candidate symbol.
 * @param[in] symbolTable Symbol table providing type inheritance hierarchy.
 * @param[in] unusedRules Active unused symbol rules.
 * @return True if symbol is invoked directly by the engine host.
 */
[[nodiscard]] bool IsEngineManagedFunction(const Symbol& sym, const SymbolTable& symbolTable,
                                           const config::UnusedSymbolRules& unusedRules);

/// Set of names viewing storage owned by the symbol table, the rule index or a utils::StringPool.
using NameViewSet = ankerl::unordered_dense::set<std::string_view>;

/**
 * @brief Tracks symbol references, enclosing scopes, and exclusion rules during unused variable detection.
 */
struct UnusedCheckContext
{
    const NameViewSet& refNames;
    const ankerl::unordered_dense::set<const LocalDefinition*>& usedLocals;
    const NameViewSet& reflections;
    const config::UnusedSymbolRules& rules;
    const NameViewSet& inheritedOrMixedInClasses;
    const ankerl::unordered_dense::map<std::string_view, std::vector<std::string_view>>& derivedClassesByBase;
    const RuleIndex& ruleIndex;
};

/**
 * @brief Context for collecting documentation references to symbols from markdown and doc comments.
 */
struct DocRefCollectContext
{
    const ankerl::unordered_dense::set<uint64_t>& defLocs;
    std::string_view sourceCode;
    /// Engine rules naming delegate properties whose member accesses are not references; null disables the filter.
    const config::UnusedSymbolRules* rules = nullptr;
};
void CollectDefLocations(const Scope* rootScope, ankerl::unordered_dense::set<uint64_t>& defLocs);

/**
 * @brief Adds every referenced name found in a scope tree, except references at definition sites and member
 *        accesses on delegate-property receivers.
 * @param[in] rootScope Root of the lexical scope tree to walk.
 * @param[in] docCtx Definition locations to skip, the source text and the rules naming delegate properties.
 * @param[in,out] refNames Receives the referenced names as views of pool storage.
 * @param[in,out] pool Owns the copies of the names, so rootScope may be released afterwards.
 */
void CollectDocReferences(const Scope* rootScope, const DocRefCollectContext& docCtx, NameViewSet& refNames,
                          utils::StringPool& pool);

/**
 * @brief Collect used locals.
 * @param[in] rootScope Lexical scope for symbol lookup.
 * @param[in] usedLocals
 * Source location or position.
 */
void CollectUsedLocals(const Scope* rootScope, ankerl::unordered_dense::set<const LocalDefinition*>& usedLocals);

/**
 * @brief Adds the names referenced by the other files of the module.
 * @param[in] req Request providing the module file list and the scope index.
 * @param[in,out] refNames Receives the referenced names as views of pool storage.
 * @param[in,out] pool Owns the copies of the names.
 */
void CollectCrossFileReferences(const SemanticAnalysisRequest& req, NameViewSet& refNames, utils::StringPool& pool);

/**
 * @brief Collects symbols referenced via string literals and reflection calls across the module.
 * @param[in] req Semantic analysis request containing scopes, rules, and node index.
 * @param[in,out] pool Owns the copies of the names, which come from temporary lookups and reflection facts.
 * @return Names identified as referenced through reflection, viewing pool storage.
 */
NameViewSet CollectStringReflection(const SemanticAnalysisRequest& req, utils::StringPool& pool);

bool IsIgnoredByDiagnosticsConfig(const Symbol& sym, const SemanticAnalysisRequest& request);

bool IsIgnoredFunction(const Symbol& sym, const SemanticAnalysisRequest& request,
                       const config::UnusedSymbolRules& rules);

void CheckLocalsAndFields(const Scope* rootScope, const UnusedCheckContext& uCtx, DiagnosticContext& ctx);

void CheckUnusedGlobals(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx);

void CheckUnusedFunctions(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx);

void CheckUnusedClasses(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis::rules
