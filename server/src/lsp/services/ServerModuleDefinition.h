#pragma once

#include "config/ServerConfig.h"
#include "lsp/Server.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace angel_lsp
{
std::optional<ModuleView> ResolveModuleDefinition(Server& server,
                                                  const config::ServerConfig::ModuleDefinition& definition,
                                                  std::span<const ModuleView> resolved,
                                                  std::span<const std::string> workspaceFiles);
} // namespace angel_lsp
