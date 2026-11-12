#pragma once

#include "config/ServerConfig.h"
#include "lsp/Server.h"

#include <optional>
#include <string>
#include <vector>

namespace angel_lsp
{
std::optional<ModuleView> ResolveModuleDefinition(Server& server,
                                                  const config::ServerConfig::ModuleDefinition& definition,
                                                  const std::vector<ModuleView>& resolved,
                                                  const std::vector<std::string>& workspaceFiles);
} // namespace angel_lsp
