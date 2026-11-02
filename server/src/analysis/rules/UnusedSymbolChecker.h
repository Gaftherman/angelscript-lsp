#pragma once

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/diagnostics/Diagnostics.h"

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
 * @param[in] signatures List of expected parameter-type lists.
 * @return True if parameter types match one of the candidate signatures.
 */
[[nodiscard]] bool MatchesEngineCallbackSignature(const Symbol& sym,
                                                  const std::vector<std::vector<std::string>>& signatures);

/**
 * @brief Checks if a class inherits from an engine base class listed in ignoredBaseClasses.
 * @param[in] className Name of the candidate class.
 * @param[in] symbolTable Symbol table providing type inheritance hierarchy.
 * @param[in] rules Active unused symbol rules.
 * @return True if class or any ancestor is an engine base class.
 */
[[nodiscard]] bool IsEngineEntity(const std::string& className, const SymbolTable& symbolTable,
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

/**
 * @brief Tracks symbol references, enclosing scopes, and exclusion rules during unused variable detection.
 */
struct UnusedCheckContext
{
    const ankerl::unordered_dense::set<std::string>& refNames;
    const ankerl::unordered_dense::set<const LocalDefinition*>& usedLocals;
    const ankerl::unordered_dense::set<std::string>& reflections;
    const config::UnusedSymbolRules& rules;
    const ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>& inheritedOrMixedInClasses;
    const ankerl::unordered_dense::map<std::string, std::vector<std::string>, TransparentStringHash, std::equal_to<>>&
        derivedClassesByBase;
    const RuleIndex& ruleIndex;
};

/**
 * @brief Context for collecting documentation references to symbols from markdown and doc comments.
 */
struct DocRefCollectContext
{
    const ankerl::unordered_dense::set<uint64_t>& defLocs;
    std::string_view sourceCode;
    std::span<const std::string> delegateProps;
};
} // namespace angel_lsp::analysis::rules
