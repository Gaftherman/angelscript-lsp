#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "utils/IncludeResolver.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a Code Action request.
 */
struct CodeActionRequest
{
    std::string_view uri;        ///< Document URI; borrowed, must outlive the request.
    std::string_view sourceCode; ///< Document text; borrowed, must outlive the request.
    lsp::Range range;
    lsp::CodeActionContext context;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    utils::IncludeSearchScope includeScope{}; ///< Include confinement roots used by include fixes; borrowed.
    const config::ServerConfig* config = nullptr;
    const i18n::I18n* i18n = nullptr;
    /// Pass-1 index of the document; providers read syntax facts from it instead of the raw tree.
    const analysis::NodeIndex* nodeIndex = nullptr;
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
