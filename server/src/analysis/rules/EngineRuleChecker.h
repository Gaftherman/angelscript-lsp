#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/EngineDialectFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"

#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{

/**
 * @brief Fast single-pass engine dialect check using pre-extracted facts.
 * @param[in] nodeIndex Prebuilt node index.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckEngineDialectRules(const NodeIndex& nodeIndex, DiagnosticContext& ctx);

/**
 * @brief Validates member and global variables against data-driven storage rules.
 * @param[in] sym The variable or property symbol.
 * @param[in] sig Variable signature.
 * @param[in] ctx Diagnostic context.
 */
void CheckEngineStorageRules(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx);

/**
 * @brief Checks member and global variables for recommended type replacements.
 * @param[in] sym The variable or property symbol.
 * @param[in] sig Variable signature.
 * @param[in] ctx Diagnostic context.
 */
void CheckEngineTypeSuggestions(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx);

/**
 * @brief Checks local variables for recommended type replacements.
 * @param[in] def Local definition structure.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckEngineLocalTypeSuggestions(const LocalDefinition& def, DiagnosticContext& ctx);

/**
 * @brief Bundled parameters for scheduler call validation.
 */
struct SchedulerCallRequest
{
    /// AST-free source span, receiver text, and argument coordinates for the call site.
    SchedulerCallSite site{};
    /// Callee method or function identifier name.
    std::string_view methodName{};
    /// Type names for each passed argument.
    std::span<const std::string_view> argTypes{};
    /// Candidate symbol overloads matching the invocation.
    const std::vector<const Symbol*>& candidates;
};

/**
 * @brief Validates an asynchronous or scheduler call expression against data-driven rules.
 * @param[in] req Scheduler call request data.
 * @param[in] ctx Diagnostic context.
 */
void CheckEngineSchedulerCall(const SchedulerCallRequest& req, const DiagnosticContext& ctx);

} // namespace angel_lsp::analysis::rules
