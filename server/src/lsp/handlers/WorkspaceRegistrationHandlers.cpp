#include "lsp/Server.h"

#include <utility>

namespace angel_lsp
{

static void RegisterLifecycleHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::Initialize>([&server](lsp::requests::Initialize::Params&& params)
                                           { return server.HandleRequestsInitialized(std::move(params)); });

    handler.add<lsp::notifications::Initialized>([&server](lsp::notifications::Initialized::Params&& params)
                                                 { server.HandleNotificationsInitialized(std::move(params)); });

    handler.add<lsp::requests::Shutdown>([&server]() { return server.HandleRequestsShutdown(); });

    handler.add<lsp::notifications::Exit>([&server]() { server.HandleNotificationsExit(); });

    handler.add<lsp::notifications::Window_WorkDoneProgress_Cancel>(
        [&server](lsp::notifications::Window_WorkDoneProgress_Cancel::Params&& params)
        { server.HandleNotificationsWindow_WorkDoneProgress_Cancel(std::move(params)); });

    handler.add<lsp::notifications::SetTrace>([&server](lsp::notifications::SetTrace::Params&& params)
                                              { server.HandleNotificationsSetTrace(std::move(params)); });

    handler.add<lsp::notifications::CancelRequest>(
        [](lsp::notifications::CancelRequest::Params&&)
        {
            // Synchronous message dispatch consumes the cancel without further action.
        });
}

static void RegisterFileEventHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::notifications::Workspace_DidCreateFiles>(
        [&server](lsp::notifications::Workspace_DidCreateFiles::Params&& params)
        { server.HandleNotificationsWorkspace_DidCreateFiles(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidRenameFiles>(
        [&server](lsp::notifications::Workspace_DidRenameFiles::Params&& params)
        { server.HandleNotificationsWorkspace_DidRenameFiles(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidDeleteFiles>(
        [&server](lsp::notifications::Workspace_DidDeleteFiles::Params&& params)
        { server.HandleNotificationsWorkspace_DidDeleteFiles(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidChangeWatchedFiles>(
        [&server](lsp::notifications::Workspace_DidChangeWatchedFiles::Params&& params)
        { server.HandleNotificationsWorkspace_DidChangeWatchedFiles(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidChangeWorkspaceFolders>(
        [&server](lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params&& params)
        { server.HandleNotificationsWorkspace_DidChangeWorkspaceFolders(std::move(params)); });
}

static void RegisterDocumentSyncHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::notifications::TextDocument_DidOpen>(
        [&server](lsp::notifications::TextDocument_DidOpen::Params&& params)
        { server.HandleNotificationsTextDocument_DidOpen(std::move(params)); });

    handler.add<lsp::notifications::TextDocument_DidChange>(
        [&server](lsp::notifications::TextDocument_DidChange::Params&& params)
        { server.HandleNotificationsTextDocument_DidChange(std::move(params)); });

    handler.add<lsp::notifications::TextDocument_DidClose>(
        [&server](lsp::notifications::TextDocument_DidClose::Params&& params)
        { server.HandleNotificationsTextDocument_DidClose(std::move(params)); });

    handler.add<lsp::notifications::TextDocument_WillSave>(
        [&server](lsp::notifications::TextDocument_WillSave::Params&& params)
        { server.HandleNotificationsTextDocument_WillSave(std::move(params)); });

    handler.add<lsp::notifications::TextDocument_DidSave>(
        [&server](lsp::notifications::TextDocument_DidSave::Params&& params)
        { server.HandleNotificationsTextDocument_DidSave(std::move(params)); });

    handler.add<lsp::requests::TextDocument_WillSaveWaitUntil>(
        [&server](lsp::requests::TextDocument_WillSaveWaitUntil::Params&& req)
        { return server.HandleRequestsTextDocument_WillSaveWaitUntil(std::move(req)); });
}

static void RegisterWorkspaceActionHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::Workspace_Diagnostic>(
        [&server](lsp::requests::Workspace_Diagnostic::Params&& params)
        { return server.HandleRequestsWorkspace_Diagnostic(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidChangeConfiguration>(
        [&server](lsp::notifications::Workspace_DidChangeConfiguration::Params&& params)
        { server.HandleNotificationsWorkspace_DidChangeConfiguration(std::move(params)); });

    handler.add<lsp::requests::Workspace_TextDocumentContent>(
        [&server](lsp::requests::Workspace_TextDocumentContent::Params&& req)
        { return server.HandleRequestsWorkspace_TextDocumentContent(std::move(req)); });

    handler.add<lsp::requests::Workspace_Symbol>([&server](lsp::requests::Workspace_Symbol::Params&& req)
                                                 { return server.HandleRequestsWorkspace_Symbol(std::move(req)); });

    handler.add<lsp::requests::WorkspaceSymbol_Resolve>(
        [&server](lsp::requests::WorkspaceSymbol_Resolve::Params&& req)
        { return server.HandleRequestsWorkspaceSymbol_Resolve(std::move(req)); });

    handler.add<lsp::requests::Workspace_ExecuteCommand>(
        [&server](lsp::requests::Workspace_ExecuteCommand::Params&& req)
        { return server.HandleRequestsWorkspace_ExecuteCommand(std::move(req)); });
}

void Server::RegisterWorkspaceHandlers()
{
    RegisterLifecycleHandlers(*this, *m_messageHandler);
    RegisterFileEventHandlers(*this, *m_messageHandler);
    RegisterDocumentSyncHandlers(*this, *m_messageHandler);
    RegisterWorkspaceActionHandlers(*this, *m_messageHandler);
}

} // namespace angel_lsp
