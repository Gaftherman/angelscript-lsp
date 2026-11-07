#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ReferenceCounting.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "utils/LspLogger.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a Code Lens request.
 */
struct CodeLensRequest
{
    std::string_view uri;
    std::string_view sourceCode;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    angel_lsp::utils::LspLogger* logger = nullptr;
    std::string_view predefinedExtension = ".as.predefined";
    const config::UnusedSymbolRules* unusedRules = nullptr;
    const i18n::I18n* i18n = nullptr;
    const config::ServerConfig* config = nullptr;
    std::string_view clientUri{};
    const analysis::ModuleFilter* occurrenceModule = nullptr;
};

/**
 * @brief Context and input parameters for resolving a Code Lens item.
 */
struct CodeLensResolveRequest
{
    const lsp::CodeLens& codeLens;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    angel_lsp::utils::LspLogger* logger = nullptr;
};

/**
 * @brief Computes code lenses (such as reference counts and interface implementations) for a document.
 * @param[in] request Immutable context for code lens generation.
 * @return List of code lenses for classes, interfaces, methods, and functions.
 */
[[nodiscard]] std::optional<std::vector<lsp::CodeLens>> GetCodeLenses(const CodeLensRequest& request);

/**
 * @brief Resolves additional command or details for a given Code Lens.
 * @param[in] request Immutable context for code lens resolution.
 * @return Resolved Code Lens.
 */
[[nodiscard]] std::optional<lsp::CodeLens> ResolveCodeLens(const CodeLensResolveRequest& request);
} // namespace angel_lsp::features
