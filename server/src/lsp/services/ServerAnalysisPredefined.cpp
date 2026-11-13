#include "lsp/Server.h"
#include "parser/SyntaxTree.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <memory>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{
bool Server::IsAnalyzeDocumentStale(const AnalyzeDocumentRequest& req) const
{
    if (IsAnalysisRequestStale(req))
    {
        return true;
    }

    const int currentVersion = GetDocumentVersion(req.uriStr);
    if (currentVersion >= 0 && req.version >= 0 && req.version != currentVersion)
    {
        return true;
    }
    if (currentVersion >= 0 && req.version < 0)
    {
        return true;
    }
    return false;
}

void Server::StorePredefinedSnapshot(const AnalyzeDocumentRequest& req, std::string_view analysisText,
                                     const parser::TreePtr& tree, bool contributes)
{
    if (contributes)
    {
        ClaimPredefinedFile(req.uriStr, true);
        m_predefinedManager.SetDocumentText(req.uriStr, std::string(analysisText));
        if (tree)
        {
            GetDocumentStore().SetTree(req.uriStr, parser::CopyTree(tree.get()));
            auto updatedDoc = std::make_shared<const document::Document>(
                document::DocumentSnapshot{req.uriStr, std::string(analysisText), req.version, req.generation},
                parser::CopyTree(tree.get()));
            m_predefinedManager.SetPreindexedDocument(req.uriStr, updatedDoc);
            if (m_workspaceStore)
            {
                m_workspaceStore->SetPreindexedDocument(req.uriStr, updatedDoc);
            }
        }
    }
    else
    {
        m_predefinedManager.RemoveStub(req.uriStr);
    }
}

bool Server::TryZeroCopyPredefinedAnalysis(const AnalyzeDocumentRequest& req, std::string_view analysisText,
                                           const utils::HighResTimer& totalTimer)
{
    auto preindexedDoc = m_workspaceStore ? m_workspaceStore->GetPreindexedDocument(req.uriStr)
                                          : m_predefinedManager.GetPreindexedDocument(req.uriStr);
    if (!preindexedDoc)
    {
        preindexedDoc = m_predefinedManager.GetPreindexedDocument(req.uriStr);
    }
    if (!preindexedDoc || !angel_lsp::utils::TextContentMatchesIgnoringLineEndings(preindexedDoc->text, analysisText))
    {
        return false;
    }
    const double totalMs = totalTimer.ElapsedMs();
    LogInfo(
        fmt::format("[Predefined Zero-Copy Fast Path] File: {} content unchanged; linked pre-indexed snapshot in {}",
                    req.uriStr, utils::FormatDuration(totalMs)));
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {} (Parse: 0.00 ms, Collector: 0.00 ms, "
                        "Scopes: 0.00 ms, Checkers: 0.00 ms)",
                        req.uriStr, utils::FormatDuration(totalMs)));
    CommitAnalysisResults({
        .uriStr = req.uriStr,
        .version = req.version,
        .generation = req.generation,
        .configRevision = req.configRevision,
        .staging = nullptr,
        .scopeRoot = nullptr,
        .calls = {},
        .diagnostics = {},
        .text = req.text,
        .closedRevision = req.closedRevision,
    });
    return true;
}

void Server::AnalyzePredefinedDocument(AnalyzeDocumentRequest req, const utils::HighResTimer& totalTimer)
{
    LogInfo(fmt::format("[Analysis] Starting predefined analysis for file: {}", req.uriStr));
    const std::string& analysisText = req.text;
    if (TryZeroCopyPredefinedAnalysis(req, analysisText, totalTimer))
    {
        return;
    }
    utils::HighResTimer parseTimer;
    parser::TreePtr tree = req.treeCopy ? std::move(req.treeCopy) : parser::MakeTreePtr(req.parser.Parse(analysisText));
    double parseMs = parseTimer.ElapsedMs();

    utils::HighResTimer colTimer;
    angel_lsp::analysis::SymbolTable staging;
    const bool contributes = PredefinedStubContributes(req.uriStr);
    if (contributes && tree)
    {
        m_symbolCollector->CollectSymbolsWithTree({req.uriStr, analysisText, m_i18n.get()}, tree.get(), staging);
    }
    double colMs = colTimer.ElapsedMs();

    StorePredefinedSnapshot(req, analysisText, tree, contributes);

    std::shared_ptr<angel_lsp::analysis::Scope> scopeRoot;
    std::vector<analysis::CallSite> calls;
    double scopeMs = 0.0;
    double checkMs = 0.0;

    const double totalMs = totalTimer.ElapsedMs();
    LogInfo(fmt::format("[Analysis] Finished predefined analysis for file: {} in {} (Parse: {}, Symbols: {})",
                        req.uriStr, utils::FormatDuration(totalMs), utils::FormatDuration(parseMs),
                        utils::FormatDuration(colMs)));
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {} (Parse: {}, Collector: {}"
                        ", Scopes: {}, Checkers: {})",
                        req.uriStr, utils::FormatDuration(totalMs), utils::FormatDuration(parseMs),
                        utils::FormatDuration(colMs), utils::FormatDuration(scopeMs), utils::FormatDuration(checkMs)));

    const bool committed = CommitAnalysisResults({
        .uriStr = req.uriStr,
        .version = req.version,
        .generation = req.generation,
        .configRevision = req.configRevision,
        .staging = &staging,
        .scopeRoot = std::move(scopeRoot),
        .calls = std::move(calls),
        .diagnostics = {},
        .text = req.text,
        .closedRevision = req.closedRevision,
    });

    if (!committed)
    {
        return;
    }

    const bool wordsChanged = RefreshStubDefinedWords(req.uriStr, req.text);
    if (wordsChanged)
    {
        ReanalyseOpenDocuments();
    }
}
} // namespace angel_lsp
