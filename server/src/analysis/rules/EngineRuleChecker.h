#pragma once

#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis::rules
{

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
    TSNode callNode;
    std::string_view methodName;
    const std::vector<TSNode>& argNodes;
    const std::vector<std::string>& argTypes;
    const std::vector<const Symbol*>& candidates;
};

/**
 * @brief Validates an asynchronous or scheduler call expression against data-driven rules.
 * @param[in] req Scheduler call request data.
 * @param[in] ctx Diagnostic context.
 */
void CheckEngineSchedulerCall(const SchedulerCallRequest& req, const DiagnosticContext& ctx);

} // namespace angel_lsp::analysis::rules
