#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"

#include <functional>
#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a definition request.
 */
struct DefinitionRequest
{
    std::string_view uri;
    std::string_view sourceCode;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    lsp::Position position;
    std::function<std::string(const std::string& rawPath)> resolveInclude = {};
    std::string_view predefinedExtension = ".as.predefined";
    const config::EngineRuleConfig* engineRules = nullptr;
    std::optional<analysis::CallBindingRevision> callBindingRevision = std::nullopt;
};

/**
 * @brief Resolves definition locations for symbol under cursor.
 * @param[in] request Immutable context for definition lookup.
 * @return Optional vector of Locations; nullopt or empty if not resolved.
 */
std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest& request);

/**
 * @brief Resolves type definition locations for symbol under cursor.
 * @param[in] request Immutable context for type definition lookup.
 * @return Optional vector of Locations pointing to type declarations.
 */
std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest& request);
} // namespace angel_lsp::features
