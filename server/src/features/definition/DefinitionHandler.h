#pragma once

#include "analysis/resolution/DefinitionResolution.h"

#include <functional>
#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Protocol context and input parameters for a definition request.
 */
struct DefinitionRequest
{
    analysis::DefinitionQuery query;
    std::function<std::optional<lsp::DocumentUri>(std::string_view rawPath)> resolveInclude = {};
};

/**
 * @brief Resolves definition locations for symbol under cursor.
 * @param[in] request Protocol request containing semantic query and include resolver.
 * @return Optional vector of Locations; nullopt or empty if not resolved.
 */
std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest& request);

/**
 * @brief Resolves type definition locations for symbol under cursor.
 * @param[in] request Protocol request containing semantic query.
 * @return Optional vector of Locations pointing to type declarations.
 */
std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest& request);

} // namespace angel_lsp::features
