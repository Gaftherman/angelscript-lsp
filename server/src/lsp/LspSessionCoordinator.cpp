#include "lsp/LspSessionCoordinator.h"

#include <lsp/connection.h>
#include <lsp/io/stream.h>
#include <lsp/messagehandler.h>
#include <string_view>

namespace angel_lsp
{

LspSessionCoordinator::LspSessionCoordinator() = default;

void LspSessionCoordinator::Start() noexcept
{
    m_running.store(true);
}

void LspSessionCoordinator::RequestShutdown() noexcept
{
    m_running.store(false);
}

bool LspSessionCoordinator::IsRunning() const noexcept
{
    return m_running.load();
}

void LspSessionCoordinator::SetWorkspaceScanComplete(bool complete) noexcept
{
    m_workspaceScanComplete.store(complete);
}

bool LspSessionCoordinator::IsWorkspaceScanComplete() const noexcept
{
    return m_workspaceScanComplete.load();
}

void LspSessionCoordinator::SetPredefinedReady(bool ready)
{
    {
        std::lock_guard<std::mutex> lock(m_predefinedMutex);
        m_predefinedReady.store(ready);
    }
    m_predefinedCv.notify_all();
}

bool LspSessionCoordinator::IsPredefinedReady() const noexcept
{
    return m_predefinedReady.load();
}

void LspSessionCoordinator::WaitUntilPredefinedReady() const
{
    if (m_predefinedReady.load())
    {
        return;
    }
    std::unique_lock<std::mutex> lock(m_predefinedMutex);
    m_predefinedCv.wait(lock, [this]() { return m_predefinedReady.load(); });
}

bool LspSessionCoordinator::WaitUntilPredefinedReady(std::chrono::milliseconds timeout) const
{
    if (m_predefinedReady.load())
    {
        return true;
    }
    std::unique_lock<std::mutex> lock(m_predefinedMutex);
    return m_predefinedCv.wait_for(lock, timeout, [this]() { return m_predefinedReady.load(); });
}

void LspSessionCoordinator::SetFormatBraceStyleKR(bool kr) noexcept
{
    m_formatBraceStyleKR.store(kr);
}

bool LspSessionCoordinator::GetFormatBraceStyleKR() const noexcept
{
    return m_formatBraceStyleKR.load();
}

uint64_t LspSessionCoordinator::IncrementConfigRevision() noexcept
{
    return ++m_configRevision;
}

uint64_t LspSessionCoordinator::GetConfigRevision() const noexcept
{
    return m_configRevision.load();
}

void LspSessionCoordinator::SetPositionEncoding(utils::PositionEncoding encoding) noexcept
{
    m_positionEncoding = encoding;
}

utils::PositionEncoding LspSessionCoordinator::GetPositionEncoding() const noexcept
{
    return m_positionEncoding;
}

void LspSessionCoordinator::SetClientSupportsSnippets(bool supported) noexcept
{
    m_clientSupportsSnippets = supported;
}

bool LspSessionCoordinator::GetClientSupportsSnippets() const noexcept
{
    return m_clientSupportsSnippets;
}

void LspSessionCoordinator::SetClientSupportsMarkdown(bool supported) noexcept
{
    m_clientSupportsMarkdown = supported;
}

bool LspSessionCoordinator::GetClientSupportsMarkdown() const noexcept
{
    return m_clientSupportsMarkdown;
}

void LspSessionCoordinator::SetClientPullsDiagnostics(bool pulls) noexcept
{
    m_clientPullsDiagnostics = pulls;
}

bool LspSessionCoordinator::GetClientPullsDiagnostics() const noexcept
{
    return m_clientPullsDiagnostics;
}

void LspSessionCoordinator::SetClientSupportsDiagnosticRefresh(bool refresh) noexcept
{
    m_clientSupportsDiagnosticRefresh = refresh;
}

bool LspSessionCoordinator::GetClientSupportsDiagnosticRefresh() const noexcept
{
    return m_clientSupportsDiagnosticRefresh;
}

bool LspSessionCoordinator::ProcessMessageStep(lsp::MessageHandler& handler)
{
    try
    {
        handler.processIncomingMessages();
        return true;
    }
    catch (const lsp::ConnectionError& e)
    {
        const std::string_view msg = e.what();
        if (msg.find("exceeds maximum allowable LSP envelope") != std::string_view::npos ||
            msg.starts_with("Protocol:"))
        {
            return false;
        }
        m_running.store(false);
        return false;
    }
    catch (const lsp::io::Error&)
    {
        m_running.store(false);
        return false;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace angel_lsp
