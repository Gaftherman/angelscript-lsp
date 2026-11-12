#include "analysis/index/NodeIndex.h"
#include "analysis/types/EngineConfigConverter.h"
#include "lsp/Server.h"
#include "parser/SyntaxTree.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <chrono>
#include <memory>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{
PreparedAnalysisSource Server::PrepareAnalysisSource(const std::string& uriStr, const std::string& text) const
{
    if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
    {
        if (angel_lsp::analysis::IsOfficialEngineConfig(text))
        {
            return {angel_lsp::analysis::ConvertEngineConfigToPredefined(text).predefinedContent, {}};
        }
        return {angel_lsp::utils::SanitizePredefinedContent(text), {}};
    }

    auto scan = utils::ScanPreprocessor(text, *DefinedWords(), m_config.preprocessor,
                                        m_config.pragmaMode != config::ServerConfig::PragmaMode::Accept);
    if (scan.excluded.empty() && scan.unsupported.empty())
    {
        static const auto empty = std::make_shared<const utils::PreprocessorScan>();
        return {text, empty};
    }
    auto parsed = scan.excluded.empty() ? text : utils::BlankExcludedLines(text, scan.excluded);
    return {std::move(parsed), std::make_shared<const utils::PreprocessorScan>(std::move(scan))};
}

void Server::DidOpenPredefinedFile(const DidOpenPredefinedRequest& req)
{
    const std::string analysisText = PrepareAnalysisSource(req.uriStr, req.text).text;
    const bool contributes = PredefinedStubContributes(req.uriStr);

    if (!contributes)
    {
        GetDocumentStore().OpenDocument(DocumentStore::OpenDocumentRequest{
            req.uriStr, req.text, req.version, parser::MakeTreePtr(nullptr), req.clientUri});
        m_symbolTable.ClearDocumentSymbols(req.uriStr);
        m_predefinedManager.RemoveStub(req.uriStr);
        if (m_workspaceStore)
        {
            m_workspaceStore->SetPreindexedDocument(req.uriStr, nullptr);
        }
        const double totalMs = req.totalTimer.ElapsedMs();
        LogInfo(fmt::format("[File Open] Predefined file opened (non-contributing): {} in {}", req.uriStr,
                            utils::FormatDuration(totalMs)));
        LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {} (Parse: 0.00 ms, Collector: 0.00 ms, "
                            "Scopes: 0.00 ms, Checkers: 0.00 ms)",
                            req.uriStr, utils::FormatDuration(totalMs)));
        PublishDiagnostics(req.uriStr, {}, req.version);
        return;
    }

    auto preindexedDoc = m_workspaceStore ? m_workspaceStore->GetPreindexedDocument(req.uriStr)
                                          : m_predefinedManager.GetPreindexedDocument(req.uriStr);
    if (!preindexedDoc)
    {
        preindexedDoc = m_predefinedManager.GetPreindexedDocument(req.uriStr);
    }

    const bool contentUnchanged =
        preindexedDoc && angel_lsp::utils::TextContentMatchesIgnoringLineEndings(preindexedDoc->text, analysisText);

    if (contentUnchanged)
    {
        GetDocumentStore().LinkDocument(req.uriStr, preindexedDoc, req.clientUri, req.version);
        const double totalMs = req.totalTimer.ElapsedMs();
        LogInfo(fmt::format("[File Open] Finished opening predefined file (zero-copy link): {} in {}", req.uriStr,
                            utils::FormatDuration(totalMs)));
        LogInfo(fmt::format("[Predefined Zero-Copy Fast Path] File: {} content unchanged; linked pre-indexed snapshot "
                            "in {}",
                            req.uriStr, utils::FormatDuration(totalMs)));
        LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {} (Parse: 0.00 ms, Collector: 0.00 ms, "
                            "Scopes: 0.00 ms, Checkers: 0.00 ms)",
                            req.uriStr, utils::FormatDuration(totalMs)));
        PublishDiagnostics(req.uriStr, {}, req.version);
        return;
    }

    parser::TreePtr tree = parser::MakeTreePtr(m_parser->Parse(analysisText));
    ReplaceSymbolsFromTree(req.uriStr, analysisText, &tree);
    CommitPredefinedIndex(req.uriStr, analysisText, parser::CopyTree(tree));
    GetDocumentStore().OpenDocument(
        DocumentStore::OpenDocumentRequest{req.uriStr, req.text, req.version, std::move(tree), req.clientUri});

    ScheduleAnalysisImmediate(ScheduleAnalysisRequest{
        .uriStr = req.uriStr,
        .text = req.text,
        .force = true,
        .version = req.version,
    });

    const double totalMs = req.totalTimer.ElapsedMs();
    LogInfo(fmt::format("[File Open] Predefined file opened and indexed: {} in {}", req.uriStr,
                        utils::FormatDuration(totalMs)));
}

static std::shared_ptr<const std::string> TransformedSource(std::string_view original, const std::string& parsed)
{
    return original == parsed ? nullptr : std::make_shared<const std::string>(parsed);
}

void Server::HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params&& params)
{
    const std::string clientUri = params.textDocument.uri.toString();
    std::string uriStr = DocumentKey(clientUri);
    const bool isPredefined = angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension);

    if (!isPredefined && m_config.features.enablePredefinedLoader && !IsPredefinedReady())
    {
        WaitForPredefinedReady(std::chrono::milliseconds(5000));
    }

    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    utils::HighResTimer totalTimer;
    const int version = params.textDocument.version;
    std::string text = std::move(params.textDocument.text);
    LogInfo(fmt::format("[File Open] Opening document: {}", uriStr));

    if (isPredefined)
    {
        DidOpenPredefinedFile(DidOpenPredefinedRequest{uriStr, text, version, clientUri, totalTimer});
        return;
    }

    const auto prepared = PrepareAnalysisSource(uriStr, text);
    utils::HighResTimer parseTimer;
    auto parsedTree = parser::MakeTreePtr(m_parser->Parse(prepared.text));
    const double parseMs = parseTimer.ElapsedMs();
    GetDocumentStore().OpenDocument(DocumentStore::OpenDocumentRequest{
        uriStr, text, version, std::move(parsedTree), clientUri, TransformedSource(text, prepared.text)});
    WithdrawClosedSnapshot(uriStr);
    utils::HighResTimer colTimer;
    const auto open = GetDocumentStore().GetDocument(uriStr);
    const auto revision = ResolveProtocolSyntax(uriStr, prepared.text, open.get());
    const auto& syntax = *revision.syntax;
    auto diagnostics = ReplaceSymbolsFromTree(uriStr, prepared.text, &syntax.tree, nullptr, &syntax.index);
    const double colMs = colTimer.ElapsedMs();
    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);

    double scopeMs = 0.0;
    double checkMs = 0.0;

    if (m_config.features.enablePredefinedLoader && !IsPredefinedReady())
    {
        LogInfo(fmt::format("Predefined stubs/workspace still indexing; publishing syntax diagnostics for {}", uriStr));
    }
    else
    {
        IndexModuleClosure(uriStr, m_sessionCoordinator->GetClientPullsDiagnostics() ? &m_protocolSyntax : nullptr);

        const analysis::CallBindingRevision bindingRevision{open->generation, version,
                                                            m_sessionCoordinator->GetConfigRevision()};
        auto semanticDiagnostics = CollectScopesAndAnalyze({.uriStr = uriStr,
                                                            .source = prepared,
                                                            .hasTree = static_cast<bool>(syntax.tree),
                                                            .outScopeMs = &scopeMs,
                                                            .outCheckMs = &checkMs,
                                                            .nodeIndex = syntax.index,
                                                            .callBindingRevision = bindingRevision});
        diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());
        AppendIncludeDiagnostics(uriStr, prepared.text, prepared.preprocessing->excluded, diagnostics);
    }

    PublishDiagnostics(uriStr, prepared.text, diagnostics, version, prepared.preprocessing);

    const double totalMs = totalTimer.ElapsedMs();
    LogInfo(fmt::format("[File Open] Finished opening document: {} in {}", uriStr, utils::FormatDuration(totalMs)));
    LogAnalysisProfile("[File Open]", uriStr,
                       AnalysisTimingProfile{totalMs, 0.0, parseMs, colMs, scopeMs, checkMs, revision.extractedNodes});
}
} // namespace angel_lsp
