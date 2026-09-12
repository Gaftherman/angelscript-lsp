#include "lsp/Server.h"
#include "utils/Utils.h"
#include "lsp/PositionCodec.h"
#include "features/workspace_symbol/WorkspaceSymbolHandler.h"
#include <fstream>

namespace angel_lsp
{
    void Server::RegisterWorkspaceHandlers()
    {
        m_messageHandler->add<lsp::requests::Initialize>(
            [this](lsp::requests::Initialize::Params &&params)
            {
                return this->HandleRequestsInitialized(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Initialized>(
            [this](lsp::notifications::Initialized::Params &&params)
            {
                this->HandleNotificationsInitialized(std::move(params));
            });

        m_messageHandler->add<lsp::requests::Shutdown>(
            [this]()
            {
                return this->HandleRequestsShutdown();
            });

        m_messageHandler->add<lsp::notifications::Exit>(
            [this]()
            {
                this->HandleNotificationsExit();
            });

        m_messageHandler->add<lsp::notifications::Window_WorkDoneProgress_Cancel>(
            [this](lsp::notifications::Window_WorkDoneProgress_Cancel::Params &&params)
            {
                this->HandleNotificationsWindow_WorkDoneProgress_Cancel(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::SetTrace>(
            [this](lsp::notifications::SetTrace::Params &&params)
            {
                this->HandleNotificationsSetTrace(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidCreateFiles>(
            [this](lsp::notifications::Workspace_DidCreateFiles::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidCreateFiles(std::move(params));
            });

        m_messageHandler->add<lsp::requests::Workspace_Diagnostic>(
            [this](lsp::requests::Workspace_Diagnostic::Params &&params)
            {
                return this->HandleRequestsWorkspace_Diagnostic(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidChangeConfiguration>(
            [this](lsp::notifications::Workspace_DidChangeConfiguration::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidChangeConfiguration(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidRenameFiles>(
            [this](lsp::notifications::Workspace_DidRenameFiles::Params &&params)
            {
                HandleNotificationsWorkspace_DidRenameFiles(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidDeleteFiles>(
            [this](lsp::notifications::Workspace_DidDeleteFiles::Params &&params)
            {
                HandleNotificationsWorkspace_DidDeleteFiles(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidChangeWatchedFiles>(
            [this](lsp::notifications::Workspace_DidChangeWatchedFiles::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidChangeWatchedFiles(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidChangeWorkspaceFolders>(
            [this](lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidChangeWorkspaceFolders(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_WillSave>(
            [this](lsp::notifications::TextDocument_WillSave::Params &&params)
            {
                this->HandleNotificationsTextDocument_WillSave(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidSave>(
            [this](lsp::notifications::TextDocument_DidSave::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidSave(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidOpen>(
            [this](lsp::notifications::TextDocument_DidOpen::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidOpen(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidChange>(
            [this](lsp::notifications::TextDocument_DidChange::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidChange(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidClose>(
            [this](lsp::notifications::TextDocument_DidClose::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidClose(std::move(params));
            });

        m_messageHandler->add<lsp::requests::Workspace_TextDocumentContent>(
            [this](lsp::requests::Workspace_TextDocumentContent::Params &&req) -> lsp::requests::Workspace_TextDocumentContent::Result
            {
                const std::string requested = req.uri.toString();
                static constexpr std::string_view k_scheme = "angelscript-predefined:";
                static constexpr std::string_view k_virtual_scheme = "angelscript-virtual:";

                if (requested.starts_with(k_virtual_scheme))
                {
                    lsp::TextDocumentContentResult result;
                    result.text = GenerateVirtualMixinDocument(requested);
                    return result;
                }

                if (!requested.starts_with(k_scheme))
                {
                    throw lsp::RequestError(lsp::MessageError::InvalidParams,
                                            "Unsupported scheme: " + requested);
                }

                const std::string wanted = angel_lsp::utils::IncludeResolver::NormalizePath(
                    std::string(std::string_view(requested).substr(k_scheme.size())));

                std::string fileUri;
                {
                    std::lock_guard<std::mutex> lock(m_predefinedMutex);
                    for (const auto &entry : m_predefinedUriByPath)
                    {
                        if (entry.first == wanted)
                        {
                            fileUri = entry.second;
                            break;
                        }
                    }
                }

                if (fileUri.empty())
                {
                    throw lsp::RequestError(lsp::MessageError::InvalidParams,
                                            "No predefined stub is loaded for: " + requested);
                }

                std::ifstream file(angel_lsp::utils::UriToPath(fileUri), std::ios::binary);
                if (!file.is_open())
                {
                    throw lsp::RequestError(lsp::MessageError::InvalidParams,
                                            "The predefined stub could not be read: " + requested);
                }

                lsp::TextDocumentContentResult result;
                result.text.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                return result;
            });

        m_messageHandler->add("angelscript/virtualDocumentContent",
            [this](lsp::json::Value &&params) -> lsp::json::Value
            {
                return this->HandleRequestsVirtualDocumentContent(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::CancelRequest>(
            [](lsp::notifications::CancelRequest::Params &&)
            {
                // Synchronous message dispatch consumes the cancel without further action.
            });

        m_messageHandler->add<lsp::requests::Workspace_Symbol>(
            [this](lsp::requests::Workspace_Symbol::Params &&req) -> lsp::requests::Workspace_Symbol::Result
            {
                if (!m_config.features.enableWorkspaceSymbols)
                {
                    return lsp::Array<lsp::SymbolInformation>{};
                }
                features::WorkspaceSymbolRequest wr{ req.query, m_symbolTable };
                auto symbols = features::GetWorkspaceSymbols(wr);
                if (symbols.has_value())
                {
                    EncodeAcrossDocuments(symbols.value());
                    return symbols.value();
                }
                return lsp::Array<lsp::SymbolInformation>{};
            });

        m_messageHandler->add<lsp::requests::WorkspaceSymbol_Resolve>(
            [this](lsp::requests::WorkspaceSymbol_Resolve::Params &&req) -> lsp::requests::WorkspaceSymbol_Resolve::Result
            {
                return this->HandleRequestsWorkspaceSymbol_Resolve(std::move(req));
            });

        m_messageHandler->add<lsp::requests::TextDocument_WillSaveWaitUntil>(
            [this](lsp::requests::TextDocument_WillSaveWaitUntil::Params &&req) -> lsp::requests::TextDocument_WillSaveWaitUntil::Result
            {
                return this->HandleRequestsTextDocument_WillSaveWaitUntil(std::move(req));
            });

        m_messageHandler->add<lsp::requests::Workspace_ExecuteCommand>(
            [this](lsp::requests::Workspace_ExecuteCommand::Params &&req) -> lsp::requests::Workspace_ExecuteCommand::Result
            {
                return this->HandleRequestsWorkspace_ExecuteCommand(std::move(req));
            });
    }
}
