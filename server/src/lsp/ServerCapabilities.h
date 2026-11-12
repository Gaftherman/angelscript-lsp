#pragma once

#include "lsp/Server.h"

#include <mutex>
#include <optional>

namespace angel_lsp
{

/**
 * @brief Extracts initial workspace roots from client initialization parameters and updates the workspace store.
 * @param[in] workspaceStore Workspace state store to update with workspace roots.
 * @param[in] runtimeConfigMutex Mutex guarding concurrent runtime configuration access.
 * @param[in] params Client Initialize request parameters.
 */
void ExtractInitialWorkspaceRoots(WorkspaceStateStore& workspaceStore, std::mutex& runtimeConfigMutex,
                                  const lsp::requests::Initialize::Params& params);

void ApplyEngineInitializationOptions(Server& server, const std::optional<lsp::LSPAny>& initOpts);

void BuildDiagnosticSeverityOverrides(DiagnosticsPipeline* pipeline, const angel_lsp::config::ServerConfig& config,
                                      utils::LspLogger* logger);

void NegotiateClientCapabilities(LspSessionCoordinator& sessionCoordinator,
                                 const lsp::ClientCapabilities& capabilities);

lsp::ServerCapabilities BuildServerCapabilities(const angel_lsp::config::ServerConfig& config,
                                                utils::PositionEncoding enc, bool usePullDiagnostics);

} // namespace angel_lsp
