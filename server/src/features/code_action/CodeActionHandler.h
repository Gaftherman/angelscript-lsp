#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a Code Action request.
 */
struct CodeActionRequest
{
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    lsp::Range range;
    lsp::CodeActionContext context;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    std::vector<std::string> allowedRoots = {};
    const config::ServerConfig* config = nullptr;
    const i18n::I18n* i18n = nullptr;
};

/**
 * @brief Context and input parameters for resolving a Code Action.
 */
struct CodeActionResolveRequest
{
    const lsp::CodeAction& action;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
};

/**
 * @brief Computes available code actions (quick-fixes, refactorings) for a given range and context.
 * @param request Immutable context for code actions.
 * @return Optional list of CodeActions; nullopt if none available.
 */
std::optional<std::vector<lsp::CodeAction>> GetCodeActions(const CodeActionRequest& request);

/**
 * @brief Resolves additional edit details or commands for an unresolved Code Action.
 * @param request Immutable context for code action resolution.
 * @return Resolved CodeAction.
 */
std::optional<lsp::CodeAction> ResolveCodeAction(const CodeActionResolveRequest& request);
} // namespace angel_lsp::features
