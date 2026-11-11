#include "i18n/i18n.h"
#include "lsp/ServerCapabilities.h"
#include "lsp/Server.h"

#include <memory>
#include <string>
#include <utility>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{

lsp::requests::Initialize::Result Server::HandleRequestsInitialized(lsp::requests::Initialize::Params&& params)
{
    ExtractInitialWorkspaceRoots(*m_workspaceStore, m_config, m_runtimeConfigMutex, params);
    ApplyEngineInitializationOptions(*this, params.initializationOptions);

    m_i18n = std::make_unique<angel_lsp::i18n::I18n>(
        params.locale.value_or(m_config.info.locale.empty() ? "en" : m_config.info.locale));

    NegotiateClientCapabilities(*m_sessionCoordinator, params.capabilities);
    const bool usePullDiagnostics =
        m_config.features.enablePullDiagnostics && m_sessionCoordinator->GetClientPullsDiagnostics();
    m_sessionCoordinator->SetClientPullsDiagnostics(usePullDiagnostics);

    const bool useUtf8 = (m_sessionCoordinator->GetPositionEncoding() == angel_lsp::utils::PositionEncoding::Utf8);
    LogInfo(fmt::format("Negotiated position encoding: {}", useUtf8 ? "utf-8" : "utf-16"));
    m_symbolTable.SetVirtualMixinDocumentsEnabled(m_config.features.enableVirtualMixinDocuments);
    m_symbolTable.SetConfiguredTypes(m_config.types.arrayTypeName, m_config.types.stringTypeName);

    BuildDiagnosticSeverityOverrides(m_diagnosticsPipeline.get(), m_config, m_logger.get());

    lsp::requests::Initialize::Result result;
    lsp::ServerInfo info;
    info.name = m_config.info.name;
    info.version = m_config.info.version;
    result.serverInfo = info;
    result.capabilities =
        BuildServerCapabilities(m_config, m_sessionCoordinator->GetPositionEncoding(), usePullDiagnostics);

    return result;
}

void Server::HandleNotificationsInitialized([[maybe_unused]] lsp::notifications::Initialized::Params&& params)
{
    StartWorkspaceScan();
}

lsp::requests::Shutdown::Result Server::HandleRequestsShutdown()
{
    m_sessionCoordinator->RequestShutdown();
    m_workspaceStop.Request();
    m_diagnosticsPipeline->StopScheduler();
    return lsp::requests::Shutdown::Result{};
}

void Server::HandleNotificationsExit()
{
    m_sessionCoordinator->RequestShutdown();
}

void Server::InitHandles()
{
    RegisterWorkspaceHandlers();
    RegisterTextDocumentHandlers();
    RegisterHierarchyHandlers();
    RegisterTokensAndFormattingHandlers();
}

} // namespace angel_lsp
