#include "lsp/Server.h"
#include "lsp/ServerCapabilities.h"
#include "lsp/pipeline/DiagnosticsPipeline.h"
#include "lsp/pipeline/LspSessionCoordinator.h"
#include "lsp/state/PositionCodec.h"
#include "lsp/state/WorkspaceStateStore.h"
#include "utils/MultiFileLogger.h"
#include "utils/Utils.h"

#include <chrono>
#include <exception>
#include <memory>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <thread>
#include <utility>

namespace angel_lsp
{

static bool BraceStyleIsKR(std::string_view style) noexcept
{
    return style == "kr" || style == "k&r" || style == "otbs" || style == "1tbs";
}

constexpr unsigned k_maxConsecutiveMessageErrors = 64;

Server::Server(const angel_lsp::config::ServerConfig& config, lsp::io::Stream& stream)
    : m_config(config), m_workspaceStore(std::make_unique<WorkspaceStateStore>()),
      m_diagnosticsPipeline(std::make_unique<DiagnosticsPipeline>()),
      m_sessionCoordinator(std::make_unique<LspSessionCoordinator>())
{
    m_sessionCoordinator->Start();
    m_sessionCoordinator->SetFormatBraceStyleKR(BraceStyleIsKR(m_config.format.braceStyle));
    m_sessionCoordinator->SetFormatSpacesInsideParentheses(m_config.format.spacesInsideParentheses);
    m_sessionCoordinator->SetFormatKeepEmptyBlocksOnSingleLine(m_config.format.keepEmptyBlocksOnSingleLine);
    if (!m_config.features.enablePredefinedLoader)
    {
        m_sessionCoordinator->SetPredefinedReady(true);
    }

    m_connection = std::make_unique<lsp::Connection>(stream);
    m_messageHandler = std::make_unique<lsp::MessageHandler>(*m_connection);

    m_logger = std::make_unique<angel_lsp::utils::LspLogger>(m_messageHandler.get());
    if (m_logger)
    {
        m_logger->SetLevel(angel_lsp::utils::ParseLogLevel(m_config.info.logLevel, angel_lsp::utils::LogLevel::Info));
    }

    m_parser = std::make_unique<angel_lsp::parser::AngelScriptParser>(m_logger.get());
    m_workerParser = std::make_unique<angel_lsp::parser::AngelScriptParser>(m_logger.get());
    m_symbolCollector = std::make_unique<angel_lsp::analysis::SymbolCollector>(m_logger.get());
    m_localScopeCollector = std::make_unique<angel_lsp::analysis::LocalScopeCollector>(m_logger.get());
    m_semanticAnalyzer = std::make_unique<angel_lsp::analysis::SemanticAnalyzer>(m_logger.get());
    m_i18n = std::make_unique<angel_lsp::i18n::I18n>(m_config.info.locale.empty() ? "en" : m_config.info.locale);

    m_workspaceStore->SetSearchDirectories(m_config.searchDirectories);
    m_workspaceStore->SetEngineProfile(m_config.engineProfile);
    m_workspaceStore->SetDefinedWordsFrom(std::string(), m_config.definedWords);

    m_diagnosticsPipeline->InitScheduler(
        [this](angel_lsp::AnalyzeRequest req)
        {
            AnalyzeDocument({.uriStr = std::move(req.uriStr),
                             .text = std::move(req.text),
                             .parser = *m_workerParser,
                             .treeCopy = std::move(req.tree),
                             .version = req.version,
                             .generation = req.generation,
                             .configRevision = req.configRevision,
                             .closedRevision = req.closedRevision,
                             .fromScheduler = true,
                             .preprocessing = std::move(req.preprocessing)});
        });

    BuildDiagnosticSeverityOverrides(m_diagnosticsPipeline.get(), m_config, m_logger.get());
    InitHandles();
}

void Server::InitDomainServices()
{
}

Server::~Server()
{
    if (m_sessionCoordinator)
    {
        m_sessionCoordinator->RequestShutdown();
    }
    if (m_diagnosticsPipeline)
    {
        m_diagnosticsPipeline->StopScheduler();
    }
    m_workspaceStop.Request();
    if (m_workspaceThread.joinable())
    {
        m_workspaceThread.join();
    }
    GetDocumentStore().Clear();
    m_predefinedManager.Clear();
    if (m_workspaceStore)
    {
        m_workspaceStore->Clear();
    }
}

void Server::DrainQueue()
{
    if (m_diagnosticsPipeline)
    {
        m_diagnosticsPipeline->DrainQueue();
    }
}

bool Server::ProcessIncomingMessageStep()
{
    try
    {
        m_messageHandler->processIncomingMessages();
        return true;
    }
    catch (const lsp::ConnectionError& e)
    {
        const std::string_view msg = e.what();
        if (msg.find("exceeds maximum allowable LSP envelope") != std::string_view::npos ||
            msg.starts_with("Protocol:"))
        {
            LogError(fmt::format("Protocol error, message discarded: {}", e.what()));
            return false;
        }
        LogInfo(fmt::format("Connection closed: {}", e.what()));
        m_sessionCoordinator->RequestShutdown();
        return false;
    }
    catch (const lsp::io::Error& e)
    {
        LogInfo(fmt::format("Transport closed: {}", e.what()));
        m_sessionCoordinator->RequestShutdown();
        return false;
    }
    catch (const lsp::json::ParseError& e)
    {
        LogError(fmt::format("Malformed JSON-RPC message discarded: {}", e.what()));
        return false;
    }
    catch (const lsp::jsonrpc::ProtocolError& e)
    {
        LogError(fmt::format("Protocol error, message discarded: {}", e.what()));
        return false;
    }
    catch (const std::exception& e)
    {
        LogError(fmt::format("Unhandled exception handling message: {}", e.what()));
        return false;
    }
    catch (...)
    {
        LogError("Unknown fatal exception in message step; recovering.");
        return false;
    }
}

void Server::Run()
{
    unsigned consecutiveErrors = 0;

    while (m_sessionCoordinator->IsRunning())
    {
        try
        {
            if (ProcessIncomingMessageStep())
            {
                consecutiveErrors = 0;
                continue;
            }

            if (!m_sessionCoordinator->IsRunning())
            {
                break;
            }

            if (++consecutiveErrors >= k_maxConsecutiveMessageErrors)
            {
                std::string err = fmt::format("Giving up after {} consecutive message errors; closing the session.",
                                              consecutiveErrors);
                LogError(err);
                angel_lsp::utils::MultiFileLogger::Instance().LogCrash(err);
                m_sessionCoordinator->RequestShutdown();
            }
        }
        catch (const std::exception& e)
        {
            LogError(fmt::format("Fatal unhandled exception in main server loop: {}", e.what()));
            angel_lsp::utils::MultiFileLogger::Instance().LogCrash(
                fmt::format("Fatal unhandled exception in main server loop: {}", e.what()));
        }
        catch (...)
        {
            LogError("Unknown fatal exception in main server loop recovered.");
            angel_lsp::utils::MultiFileLogger::Instance().LogCrash(
                "Unknown fatal exception in main server loop recovered.");
        }
    }
}

} // namespace angel_lsp
