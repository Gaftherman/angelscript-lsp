#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "lsp/Server.h"
#include "lsp/ServerCapabilities.h"

#include <spdlog/fmt/fmt.h>
#include <string>
#include <vector>

namespace angel_lsp
{

void NegotiateClientCapabilities(LspSessionCoordinator& sessionCoordinator, const lsp::ClientCapabilities& capabilities)
{
    utils::PositionEncoding positionEncoding = angel_lsp::utils::PositionEncoding::Utf16;
    if (capabilities.general.has_value() && capabilities.general->positionEncodings.has_value())
    {
        for (const auto& offered : capabilities.general->positionEncodings.value())
        {
            if (offered == lsp::PositionEncodingKind::UTF8)
            {
                positionEncoding = angel_lsp::utils::PositionEncoding::Utf8;
                break;
            }
        }
    }
    sessionCoordinator.SetPositionEncoding(positionEncoding);

    if (capabilities.window.has_value() && capabilities.window->workDoneProgress.has_value())
    {
        sessionCoordinator.SetWorkDoneProgressSupport(capabilities.window->workDoneProgress.value());
    }

    sessionCoordinator.SetClientPullsDiagnostics(capabilities.textDocument.has_value() &&
                                                 capabilities.textDocument->diagnostic.has_value());

    sessionCoordinator.SetClientSupportsDiagnosticRefresh(
        capabilities.workspace.has_value() && capabilities.workspace->diagnostics.has_value() &&
        capabilities.workspace->diagnostics->refreshSupport.has_value() &&
        capabilities.workspace->diagnostics->refreshSupport.value());

    if (capabilities.textDocument.has_value() && capabilities.textDocument->completion.has_value() &&
        capabilities.textDocument->completion->completionItem.has_value() &&
        capabilities.textDocument->completion->completionItem->snippetSupport.has_value())
    {
        sessionCoordinator.SetClientSupportsSnippets(
            capabilities.textDocument->completion->completionItem->snippetSupport.value());
    }
}

static void ConfigureNavigationAndEditingCapabilities(const angel_lsp::config::ServerConfig& config,
                                                      lsp::ServerCapabilities& caps)
{
    if (config.features.enableHover)
    {
        caps.hoverProvider = true;
    }

    if (config.features.enableDefinition)
    {
        caps.definitionProvider = true;
        caps.typeDefinitionProvider = true;
        caps.declarationProvider = true;
        caps.monikerProvider = true;
    }

    if (config.features.enableImplementation)
    {
        caps.implementationProvider = true;
    }

    if (config.features.enableSelectionRange)
    {
        caps.selectionRangeProvider = true;
    }

    if (config.features.enableCallHierarchy)
    {
        caps.callHierarchyProvider = true;
    }

    if (config.features.enableTypeHierarchy)
    {
        caps.typeHierarchyProvider = true;
    }

    if (config.features.enableLinkedEditing)
    {
        caps.linkedEditingRangeProvider = true;
    }

    if (config.features.enableCodeLens)
    {
        lsp::CodeLensOptions codeLensOpts;
        codeLensOpts.resolveProvider = true;
        caps.codeLensProvider = codeLensOpts;
    }
}

static void ConfigureEditingAndSymbolCapabilities(const angel_lsp::config::ServerConfig& config,
                                                  lsp::ServerCapabilities& caps)
{
    if (config.features.enableCompletion)
    {
        lsp::CompletionOptions completionOpts;
        completionOpts.triggerCharacters = lsp::Array<lsp::String>{".", ":"};
        completionOpts.resolveProvider = true;
        caps.completionProvider = completionOpts;
    }

    if (config.features.enableSemanticTokens)
    {
        lsp::SemanticTokensOptions semOpts;
        semOpts.legend = features::GetSemanticTokensLegend();
        lsp::SemanticTokensFullDelta fullDelta;
        fullDelta.delta = true;
        semOpts.full = fullDelta;
        semOpts.range = true;
        caps.semanticTokensProvider = semOpts;
    }

    if (config.features.enableSignatureHelp)
    {
        lsp::SignatureHelpOptions sigOpts;
        sigOpts.triggerCharacters = lsp::Array<lsp::String>{"(", ","};
        caps.signatureHelpProvider = sigOpts;
    }

    if (config.features.enableDocumentSymbols)
    {
        caps.documentSymbolProvider = true;
    }

    if (config.features.enableWorkspaceSymbols)
    {
        lsp::WorkspaceSymbolOptions wsOpts;
        wsOpts.resolveProvider = true;
        caps.workspaceSymbolProvider = wsOpts;
    }

    if (config.features.enableReferences)
    {
        caps.referencesProvider = true;
    }

    if (config.features.enableRename)
    {
        lsp::RenameOptions renameOpts;
        renameOpts.prepareProvider = true;
        caps.renameProvider = renameOpts;
    }

    if (config.features.enableDocumentHighlight)
    {
        caps.documentHighlightProvider = true;
    }

    if (config.features.enableFoldingRange)
    {
        caps.foldingRangeProvider = true;
    }

    if (config.features.enableInlayHints)
    {
        lsp::InlayHintOptions inlayOpts;
        inlayOpts.resolveProvider = true;
        caps.inlayHintProvider = inlayOpts;
    }
}

static void ConfigureFormattingAndDiagnosticCapabilities(const angel_lsp::config::ServerConfig& config,
                                                         lsp::ServerCapabilities& caps, bool usePullDiagnostics)
{
    if (config.features.enableCodeAction)
    {
        lsp::CodeActionOptions codeActionOpts;
        codeActionOpts.resolveProvider = true;
        codeActionOpts.codeActionKinds =
            lsp::Array<lsp::CodeActionKindEnum>{lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix),
                                                lsp::CodeActionKindEnum(lsp::CodeActionKind::Refactor),
                                                lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract),
                                                lsp::CodeActionKindEnum(lsp::CodeActionKind::SourceOrganizeImports)};
        caps.codeActionProvider = codeActionOpts;
    }

    if (config.features.enableFormatting)
    {
        caps.documentFormattingProvider = true;
        lsp::DocumentRangeFormattingOptions rangeOpts;
        rangeOpts.rangesSupport = true;
        caps.documentRangeFormattingProvider = rangeOpts;
        if (config.features.enableOnTypeFormatting)
        {
            lsp::DocumentOnTypeFormattingOptions onTypeOpts;
            onTypeOpts.firstTriggerCharacter = ";";
            onTypeOpts.moreTriggerCharacter = lsp::Array<lsp::String>{"}"};
            caps.documentOnTypeFormattingProvider = onTypeOpts;
        }
    }

    if (config.features.enableDocumentLink)
    {
        lsp::DocumentLinkOptions linkOpts;
        linkOpts.resolveProvider = true;
        caps.documentLinkProvider = linkOpts;
    }

    if (usePullDiagnostics)
    {
        lsp::DiagnosticOptions diagnosticOpts;
        diagnosticOpts.identifier = std::string("angelscript");
        diagnosticOpts.interFileDependencies = true;
        diagnosticOpts.workspaceDiagnostics = true;
        caps.diagnosticProvider = diagnosticOpts;
    }
}

static void ConfigureWorkspaceCapabilities(const angel_lsp::config::ServerConfig& config, lsp::ServerCapabilities& caps)
{
    lsp::WorkspaceFoldersServerCapabilities folderCaps;
    folderCaps.supported = true;
    folderCaps.changeNotifications = true;

    lsp::WorkspaceOptions workspaceOpts;
    workspaceOpts.workspaceFolders = folderCaps;

    lsp::FileOperationPattern scriptPattern;
    scriptPattern.glob = fmt::format("**/*{}", config.info.fileExtension);

    lsp::FileOperationFilter scriptFilter;
    scriptFilter.pattern = scriptPattern;
    scriptFilter.scheme = std::string("file");

    lsp::FileOperationRegistrationOptions registration;
    registration.filters = lsp::Array<lsp::FileOperationFilter>{scriptFilter};

    lsp::FileOperationOptions fileOps;
    fileOps.didCreate = registration;
    fileOps.didRename = registration;
    fileOps.didDelete = registration;
    workspaceOpts.fileOperations = fileOps;

    lsp::TextDocumentContentOptions contentOpts;
    contentOpts.schemes = lsp::Array<lsp::String>{"angelscript-predefined"};
    workspaceOpts.textDocumentContent = contentOpts;

    caps.workspace = workspaceOpts;

    lsp::ExecuteCommandOptions cmdOpts;
    cmdOpts.commands = lsp::Array<lsp::String>{"angelscript.listPredefinedStubs"};
    caps.executeCommandProvider = cmdOpts;
}

lsp::ServerCapabilities BuildServerCapabilities(const angel_lsp::config::ServerConfig& config,
                                                utils::PositionEncoding enc, bool usePullDiagnostics)
{
    lsp::ServerCapabilities caps;
    caps.positionEncoding = (enc == angel_lsp::utils::PositionEncoding::Utf8) ? lsp::PositionEncodingKind::UTF8
                                                                              : lsp::PositionEncodingKind::UTF16;

    lsp::TextDocumentSyncOptions sync;
    sync.openClose = true;
    sync.change = lsp::TextDocumentSyncKind::Incremental;
    sync.willSave = true;
    sync.willSaveWaitUntil = true;

    lsp::SaveOptions saveOptions;
    saveOptions.includeText = true;
    sync.save = saveOptions;
    caps.textDocumentSync = sync;

    ConfigureNavigationAndEditingCapabilities(config, caps);
    ConfigureEditingAndSymbolCapabilities(config, caps);
    ConfigureFormattingAndDiagnosticCapabilities(config, caps, usePullDiagnostics);
    ConfigureWorkspaceCapabilities(config, caps);

    return caps;
}

} // namespace angel_lsp
