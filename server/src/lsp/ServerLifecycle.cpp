#include "lsp/Server.h"
#include "lsp/pipeline/DiagnosticsPipeline.h"
#include "lsp/pipeline/LspSessionCoordinator.h"
#include "lsp/state/PositionCodec.h"
#include "lsp/state/WorkspaceStateStore.h"
#include "utils/MultiFileLogger.h"
#include "utils/Utils.h"

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{

/**
 * @brief How many message errors in a row are tolerated before the session is closed.
 *        A malformed frame is recoverable and must not end the session, but a stream that fails
 *        continually is broken and spinning on it burns CPU while filling logs.
 */
constexpr unsigned k_maxConsecutiveMessageErrors = 64;

Server::Server(const angel_lsp::config::ServerConfig& config, lsp::io::Stream& stream)
    : m_config(config),
      m_logger(std::make_unique<utils::LspLogger>(m_config.logging.channelLogDirs)),
      m_i18n(std::make_unique<i18n::I18nManager>(config.info.locale)),
      m_workspaceStore(std::make_unique<WorkspaceStateStore>()),
      m_diagnosticsPipeline(std::make_unique<DiagnosticsPipeline>()),
      m_sessionCoordinator(std::make_unique<LspSessionCoordinator>()),
      m_scheduler(std::make_unique<AnalysisScheduler>(
          *m_logger, [this](AnalysisDocumentRequest req) { ExecuteAnalysisPass(std::move(req)); })),
      m_ioStream(stream),
      m_messageHandler(std::make_unique<lsp::MessageHandler>(m_ioStream)),
      m_predefinedManager(PredefinedStubManager::Create(
          m_symbolTable, *m_workspaceStore, m_runtimeConfigMutex, m_discoveredPredefined, m_effectivePredefined)),
      m_effectivePredefined(config.activePredefined)
{
    InitDomainServices();
    InitHandles();
}

void Server::InitDomainServices()
{
    m_sessionCoordinator->SetFormatBraceStyleKR(m_config.format.braceStyle == "kr");
    m_sessionCoordinator->SetFormatSpacesInsideParentheses(m_config.format.spacesInsideParentheses);
    m_sessionCoordinator->SetFormatKeepEmptyBlocksOnSingleLine(m_config.format.keepEmptyBlocksOnSingleLine);
    m_sessionCoordinator->SetFormatPointerAlignment(m_config.format.pointerAlignment);
    m_diagnosticsPipeline->SetPublishCallback(
        [this](const std::string& uri, std::vector<lsp::Diagnostic> diags)
        {
            PublishDiagnostics(uri, std::move(diags));
        });
    m_diagnosticsPipeline->SetRefreshCallback([this]() { RefreshClientDiagnostics(); });
    m_diagnosticsPipeline->SetClientPullsCallback(
        [this]() { return m_sessionCoordinator->GetClientPullsDiagnostics(); });
    m_diagnosticsPipeline->SetClientSupportsRefreshCallback(
        [this]() { return m_sessionCoordinator->GetClientSupportsDiagnosticRefresh(); });
}

Server::~Server()
{
    // The coordinator's stop flag unblocks any thread waiting on a conditional variable, and
    // must be raised before those threads are joined - otherwise joining a worker that is
    // waiting on an empty queue hangs the server.
    m_workspaceStop.Request();
    if (m_workspaceThread.joinable())
    {
        m_workspaceThread.join();
    }

    if (m_scheduler)
    {
        m_scheduler->Stop();
    }

    if (m_parser)
    {
        m_parser.reset();
    }

    if (m_messageHandler)
    {
        m_messageHandler.reset();
    }
}

void Server::DrainQueue()
{
    while (m_ioStream.hasPendingData())
    {
        ProcessIncomingMessageStep();
    }
}

bool Server::ProcessIncomingMessageStep()
{
    try
    {
        lsp::MessageError error = m_messageHandler->processIncomingMessageStep();
        if (error == lsp::MessageError::NoError)
        {
            m_consecutiveMessageErrors = 0;
            return true;
        }

        if (error == lsp::MessageError::ConnectionClosed)
        {
            LogInfo("Client closed the connection stream; terminating message processing loop.");
            return false;
        }

        m_consecutiveMessageErrors++;
        LogWarning(fmt::format("Recoverable LSP protocol framing error encountered (code: {}, streak: {}/{})",
                               static_cast<int>(error), m_consecutiveMessageErrors, k_maxConsecutiveMessageErrors));

        if (m_consecutiveMessageErrors >= k_maxConsecutiveMessageErrors)
        {
            LogError(fmt::format(
                "Excessive consecutive protocol framing errors ({}); aborting connection loop to protect CPU.",
                m_consecutiveMessageErrors));
            return false;
        }

        return true;
    }
    catch (const std::exception& e)
    {
        LogError(fmt::format("Unhandled standard exception in message loop step: {}", e.what()));
        angel_lsp::utils::MultiFileLogger::Instance().LogCrash(
            fmt::format("Unhandled exception in processIncomingMessageStep: {}", e.what()));
        return false;
    }
    catch (...)
    {
        LogError("Unknown non-standard exception trapped in message loop step; halting.");
        angel_lsp::utils::MultiFileLogger::Instance().LogCrash("Unknown exception in processIncomingMessageStep");
        return false;
    }
}

void Server::Run()
{
    try
    {
        LogInfo("AngelLSP Server event dispatch loop started successfully.");
        while (!m_hasShutdown)
        {
            if (!ProcessIncomingMessageStep())
            {
                break;
            }
        }
        LogInfo("AngelLSP Server event dispatch loop exited cleanly.");
    }
    catch (const std::exception& e)
    {
        LogError(fmt::format("Fatal exception in main server Run loop: {}", e.what()));
        angel_lsp::utils::MultiFileLogger::Instance().LogCrash(fmt::format("Fatal crash in Server::Run: {}", e.what()));
    }
    catch (...)
    {
        LogError("Fatal unknown crash in main server Run loop.");
        angel_lsp::utils::MultiFileLogger::Instance().LogCrash("Fatal unknown crash in Server::Run");
    }
}

} // namespace angel_lsp
