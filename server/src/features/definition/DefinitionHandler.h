#pragma once

#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include <functional>
#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a definition request.
 */
struct DefinitionRequest
{
    const std::string& uri;
    const std::string& sourceCode;
    TSTree* tree = nullptr;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    lsp::Position position;
    std::function<std::string(const std::string& rawPath)> resolveInclude = {};
};

/**
 * @brief Resolves definition locations for symbol under cursor.
 * @param request Immutable context for definition lookup.
 * @return Optional vector of Locations; nullopt or empty if not resolved.
 */
std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest& request);

/**
 * @brief Resolves type definition locations for symbol under cursor.
 * @param request Immutable context for type definition lookup.
 * @return Optional vector of Locations pointing to type declarations.
 */
std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest& request);
} // namespace angel_lsp::features
