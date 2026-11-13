#include "lsp/Server.h"
#include "utils/Utils.h"

#include <string>
#include <utility>
#include <variant>

namespace angel_lsp
{

void Server::BeginWorkspaceProgress(std::string_view title)
{
    if (!m_sessionCoordinator->GetWorkDoneProgressSupport())
    {
        return;
    }

    // A fresh token per scan. RestartWorkspaceScan can begin a second scan after a folder
    // change, and reusing a token would have the client fold the two into one bar that never
    // ends.
    m_workspaceProgressToken = "angelscript-workspace-scan-" + std::to_string(++m_workspaceProgressCounter);

    lsp::WorkDoneProgressCreateParams createParams;
    createParams.token = m_workspaceProgressToken;
    // Fire and forget: the client either makes room for the token or it does not, and either
    // way the scan carries on. Waiting on the response here would block the thread doing the
    // work for no benefit.
    m_messageHandler->sendRequest<lsp::requests::Window_WorkDoneProgress_Create>(
        std::move(createParams), [](auto&&) {}, [](const auto&) {});

    lsp::WorkDoneProgressBegin begin;
    begin.title = title;
    // Cancellable now that window/workDoneProgress/cancel is answered. Announcing it while
    // nothing handled the notification would have shown the user a cancel button that did
    // nothing, which is worse than no button.
    begin.cancellable = true;
    begin.percentage = 0u;

    lsp::notifications::Progress::Params params;
    params.token = m_workspaceProgressToken;
    params.value = lsp::toJson(std::move(begin));
    m_messageHandler->sendNotification<lsp::notifications::Progress>(std::move(params));
}

void Server::ReportWorkspaceProgress(std::string_view message, unsigned percentage)
{
    if (!m_sessionCoordinator->GetWorkDoneProgressSupport() || m_workspaceProgressToken.empty())
    {
        return;
    }

    lsp::WorkDoneProgressReport report;
    report.message = message;
    report.percentage = percentage;

    lsp::notifications::Progress::Params params;
    params.token = m_workspaceProgressToken;
    params.value = lsp::toJson(std::move(report));
    m_messageHandler->sendNotification<lsp::notifications::Progress>(std::move(params));
}

void Server::EndWorkspaceProgress(std::string_view message)
{
    if (!m_sessionCoordinator->GetWorkDoneProgressSupport() || m_workspaceProgressToken.empty())
    {
        return;
    }

    lsp::WorkDoneProgressEnd end;
    end.message = message;

    lsp::notifications::Progress::Params params;
    params.token = m_workspaceProgressToken;
    params.value = lsp::toJson(std::move(end));
    m_messageHandler->sendNotification<lsp::notifications::Progress>(std::move(params));

    // Cleared so a Report arriving after the End - from a scan being torn down - is dropped
    // rather than reopening a finished bar.
    m_workspaceProgressToken.clear();
}

void Server::HandleNotificationsWindow_WorkDoneProgress_Cancel(
    lsp::notifications::Window_WorkDoneProgress_Cancel::Params&& params)
{
    // The workspace scan already polls a stop flag on every file - it has to, so a folder change
    // or a shutdown can interrupt it - so honouring a cancel is a matter of setting that flag
    // rather than of building anything. What was missing was the notification, and the
    // `cancellable` flag on the progress begin, which was false.
    //
    // Compared against the token this scan announced. A client may run several progress
    // operations at once and cancel any of them; cancelling on token alone would have stopped
    // the scan because something unrelated was dismissed.
    if (std::holds_alternative<lsp::String>(params.token) &&
        std::get<lsp::String>(params.token) == m_workspaceProgressToken)
    {
        m_workspaceStop.Request();
    }
}

void Server::HandleNotificationsSetTrace(lsp::notifications::SetTrace::Params&& params)
{
    // `$/setTrace` is how a client turns verbose logging on without restarting the server, which
    // is the difference between a user being able to send a useful log and having to reproduce
    // the problem twice.
    //
    // The protocol's three values do not line up with this server's five levels, so they are
    // mapped rather than parsed: `off` is the quietest setting that still reports real
    // failures, and both verbose steps below map onto what this server actually distinguishes.
    if (!m_logger)
        return;

    switch (params.value)
    {
    case lsp::TraceValue::Off:
        m_logger->SetLevel(utils::LogLevel::Error);
        break;
    case lsp::TraceValue::Messages:
        m_logger->SetLevel(utils::LogLevel::Info);
        break;
    case lsp::TraceValue::Verbose:
        m_logger->SetLevel(utils::LogLevel::Debug);
        break;

    // Not a value any client sends: the generator appends MAX_VALUE to every enum it writes,
    // and the framework parks a string it did not recognise there. Leaving the level where the
    // user last put it is the right answer to "I do not know what you asked for" - named rather
    // than reached by falling out of the switch, so the compiler stops warning that this enum
    // has an unhandled member and can warn about one that matters.
    case lsp::TraceValue::MAX_VALUE:
        break;
    }
}

} // namespace angel_lsp
