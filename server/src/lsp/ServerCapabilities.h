#pragma once

#include "lsp/Server.h"

#include <mutex>
#include <optional>

namespace angel_lsp
{

void ExtractInitialWorkspaceRoots(WorkspaceStateStore& workspaceStore,
                                  const angel_lsp::config::ServerConfig& config,
                                  std::mutex& runtimeConfigMutex,
                                  const lsp::requests::Initialize::Params& params);

void ApplyEngineInitializationOptions(Server& server, const std::optional<lsp::LSPAny>& initOpts);

void BuildDiagnosticSeverityOverrides(DiagnosticsPipeline* pipeline,
                                     const angel_lsp::config::ServerConfig& config,
                                     utils::LspLogger* logger);

void NegotiateClientCapabilities(LspSessionCoordinator& sessionCoordinator,
                                const lsp::ClientCapabilities& capabilities);

lsp::ServerCapabilities BuildServerCapabilities(const angel_lsp::config::ServerConfig& config,
                                               utils::PositionEncoding enc,
                                               bool usePullDiagnostics);

} // namespace angel_lsp
