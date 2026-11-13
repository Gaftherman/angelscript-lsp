#include "analysis/index/NodeIndex.h"
#include "lsp/Server.h"
#include "parser/SyntaxTree.h"
#include "utils/IncludeResolver.h"
#include "utils/Timer.h"
#include "utils/TransparentHash.h"
#include "utils/Utils.h"

#include <spdlog/fmt/fmt.h>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{
void Server::HandleNotificationsTextDocument_WillSave(
    [[maybe_unused]] lsp::notifications::TextDocument_WillSave::Params&& params)
{
}

void Server::DidSavePredefinedFile(std::string_view uriStr, std::string_view text)
{
    const std::string uri(uriStr);
    const std::string analysisText = PrepareAnalysisSource(uriStr, text).text;
    parser::TreePtr savedTree = parser::MakeTreePtr(m_parser->Parse(analysisText));

    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        m_closureDocuments.insert_or_assign(
            std::string(uriStr),
            ClosedDocumentSnapshot{std::make_shared<const std::string>(text), ++m_nextClosedRevision});
    }

    if (PredefinedStubContributes(uriStr))
    {
        ClaimPredefinedFile(uriStr, true);
        m_predefinedManager.SetDocumentText(uriStr, analysisText);
        ReplaceSymbolsFromTree(uriStr, analysisText, &savedTree);
    }
    else
    {
        m_symbolTable.ClearDocumentSymbols(uri);
        m_predefinedManager.RemoveStub(uriStr);
    }

    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);

    const bool wordsChanged = RefreshStubDefinedWords(uriStr, text);

    PublishDiagnostics(uriStr, {});
    if (!wordsChanged)
    {
        AnalyzeConfiguredModules();
    }
}

void Server::ReanalyzeDependentsOnSave(std::string_view uriStr)
{
    const std::string savedPath = CanonicalPathFromUri(uriStr);
    const ModuleClaim savedClaim = !savedPath.empty() ? ClaimFor(savedPath) : ModuleClaim{};
    std::vector<std::string> closureFiles;
    if (!savedPath.empty())
    {
        closureFiles = GetIncludeGraph().GetModuleClosure(savedPath);
    }
    TransparentStringSet closureSet(closureFiles.begin(), closureFiles.end());

    for (const auto& [openUri, openText] : GetDocumentStore().GetSnapshot())
    {
        if (openUri == uriStr)
        {
            continue;
        }

        bool isDependent = false;
        const std::string openPath = CanonicalPathFromUri(openUri);

        if (savedClaim.owner != nullptr && !openPath.empty())
        {
            const ModuleClaim openClaim = ClaimFor(openPath);
            if (openClaim.owner == savedClaim.owner)
            {
                isDependent = true;
            }
        }

        if (!isDependent && !openPath.empty() && closureSet.contains(openPath))
        {
            isDependent = true;
        }

        if (isDependent)
        {
            if (m_diagnosticsPipeline->ShouldDebouncePeer(openUri))
            {
                continue;
            }

            IndexModuleClosure(openUri);
            ScheduleAnalysis(openUri, openText);
        }
    }

    ReanalyzeClosedModuleDependents(uriStr, savedClaim, closureSet);
}

SyntaxRevision Server::ResolveProtocolSyntax(std::string_view uri, std::string_view source,
                                             const document::Document* open)
{
    const bool hasExactTree = open && open->tree && open->ParsedSource() == source;
    return ResolveSyntaxRevision({.key = {uri, source, open ? open->generation : 0},
                                  .parser = *m_parser,
                                  .exactTreeToCopy = hasExactTree ? &open->tree : nullptr},
                                 &m_protocolSyntax);
}

void Server::HandleNotificationsTextDocument_DidSave(lsp::notifications::TextDocument_DidSave::Params&& params)
{
    std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMutex);
    utils::HighResTimer totalTimer;
    std::string uriStr = DocumentKey(params.textDocument.uri.toString());
    LogInfo(fmt::format("[File Save] Processing save and re-analysis for: {}", uriStr));
    GetDocumentStore().SetClientUri(uriStr, params.textDocument.uri.toString());
    std::string text = params.text.has_value() ? params.text.value() : "";

    if (text.empty())
    {
        if (auto docText = GetDocumentStore().GetText(uriStr))
        {
            text = std::move(*docText);
        }
    }

    const int version = GetDocumentStore().GetVersion(uriStr);
    m_diagnosticsPipeline->MarkSaved(uriStr, version);

    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);

    if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
    {
        const auto previousWords = DefinedWords();
        DidSavePredefinedFile(uriStr, text);
        lifecycleLock.unlock();
        if (previousWords != DefinedWords())
        {
            RestartWorkspaceScan();
        }
        return;
    }

    const auto prepared = PrepareAnalysisSource(uriStr, text);
    const auto savedDocument = GetDocumentStore().GetDocument(uriStr);
    const auto revision = ResolveProtocolSyntax(uriStr, prepared.text, savedDocument.get());
    const auto& syntax = *revision.syntax;

    UpdateSavedDocumentCaches(uriStr, text, prepared.preprocessing->excluded);

    bool interfaceChanged = false;
    auto diagnostics = ReplaceSymbolsFromTree(uriStr, prepared.text, &syntax.tree, &interfaceChanged, &syntax.index);

    const auto bindingRevision =
        savedDocument && savedDocument->text == text
            ? std::optional(analysis::CallBindingRevision{savedDocument->generation, savedDocument->version,
                                                          m_sessionCoordinator->GetConfigRevision()})
            : std::nullopt;
    auto semanticDiagnostics = CollectScopesAndAnalyze({.uriStr = uriStr,
                                                        .source = prepared,
                                                        .hasTree = static_cast<bool>(syntax.tree),
                                                        .nodeIndex = syntax.index,
                                                        .callBindingRevision = bindingRevision});
    diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

    AppendIncludeDiagnostics(uriStr, prepared.text, prepared.preprocessing->excluded, diagnostics);

    PublishDiagnostics(uriStr, prepared.text, diagnostics, version, prepared.preprocessing);

    LogInfo(fmt::format("[File Save] Finished save analysis for: {} in {} (AST extraction nodes: {})", uriStr,
                        utils::FormatDuration(totalTimer.ElapsedMs()), revision.extractedNodes));

    HandleSavedInterfaceChange(uriStr, interfaceChanged);
}

void Server::UpdateSavedDocumentCaches(std::string_view uriStr, std::string_view text,
                                       const std::vector<utils::ExcludedLineRange>& excluded)
{
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        m_closureDocuments.insert_or_assign(
            std::string(uriStr),
            ClosedDocumentSnapshot{std::make_shared<const std::string>(text), ++m_nextClosedRevision});
    }

    if (const std::string savedPath = CanonicalPathFromUri(uriStr); !savedPath.empty())
    {
        GetIncludeGraph().UpdateFile(
            utils::WorkspaceIncludeGraph::UpdateFileRequest{savedPath,
                                                            text,
                                                            *SearchDirectories(),
                                                            IncludeAllowedRoots(),
                                                            std::string(ImplicitIncludeExtension()),
                                                            {DefinedWords(), m_config.preprocessor},
                                                            &excluded});
    }

    IndexModuleClosure(uriStr);
}

void Server::HandleSavedInterfaceChange(std::string_view uriStr, bool interfaceChanged)
{
    if (!m_modules.empty())
    {
        WithdrawStaleModuleDiagnostics();
    }

    if (interfaceChanged)
    {
        ClearMixinCaches();
        ReanalyzeDependentsOnSave(uriStr);
    }
    else
    {
        LogInfo(fmt::format("Save {}: public interface unchanged, skipping cascading re-analysis", uriStr));
    }
}
} // namespace angel_lsp
