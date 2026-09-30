#include "analysis/NodeIndex.h"
#include "lsp/PositionCodec.h"
#include "lsp/Server.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include <fstream>
#include <iterator>
#include <spdlog/fmt/fmt.h>
#include <sstream>
#include <tree_sitter/api.h>

namespace angel_lsp
{
namespace
{
TSInputEdit ComputeTsInputEdit(const std::string& buffer, const lsp::TextDocumentContentChangePartial& rt,
                               angel_lsp::utils::PositionEncoding encoding)
{
    uint32_t startLine = rt.range.start.line;
    // rt.range is expressed in the negotiated encoding, while TSPoint::column and every
    // byte offset below are byte columns. Converting here is what keeps the server's
    // buffer in step with the editor on documents containing non-ASCII text.
    uint32_t startChar = angel_lsp::utils::LspCharToByteColumn(angel_lsp::utils::GetLine(buffer, startLine),
                                                               rt.range.start.character, encoding);
    uint32_t endLine = rt.range.end.line;
    uint32_t endChar = angel_lsp::utils::LspCharToByteColumn(angel_lsp::utils::GetLine(buffer, endLine),
                                                             rt.range.end.character, encoding);

    uint32_t start_byte = static_cast<uint32_t>(
        angel_lsp::utils::PositionToOffset(buffer, startLine, rt.range.start.character, encoding));
    uint32_t old_end_byte =
        static_cast<uint32_t>(angel_lsp::utils::PositionToOffset(buffer, endLine, rt.range.end.character, encoding));
    uint32_t new_end_byte = static_cast<uint32_t>(start_byte + rt.text.size());

    TSPoint start_point = {startLine, startChar};
    TSPoint old_end_point = {endLine, endChar};

    uint32_t newlineCount = 0;
    size_t lastNewlinePos = std::string::npos;
    for (size_t i = 0; i < rt.text.size(); ++i)
    {
        if (rt.text[i] == '\n')
        {
            newlineCount++;
            lastNewlinePos = i;
        }
    }

    TSPoint new_end_point;
    if (newlineCount > 0)
    {
        new_end_point.row = startLine + newlineCount;
        new_end_point.column = static_cast<uint32_t>(rt.text.size() - (lastNewlinePos + 1));
    }
    else
    {
        new_end_point.row = startLine;
        new_end_point.column = static_cast<uint32_t>(startChar + rt.text.size());
    }

    TSInputEdit edit;
    edit.start_byte = start_byte;
    edit.old_end_byte = old_end_byte;
    edit.new_end_byte = new_end_byte;
    edit.start_point = start_point;
    edit.old_end_point = old_end_point;
    edit.new_end_point = new_end_point;
    return edit;
}
} // namespace

void Server::HandleNotificationsTextDocument_WillSave(
    [[maybe_unused]] lsp::notifications::TextDocument_WillSave::Params&& params)
{
    // The server has nothing it must do before a save - the analysis is already current and the
    // document text is already held - so the handler body is empty apart from this comment.
}

void Server::DidSavePredefinedFile(const std::string& uriStr, const std::string& text)
{
    const std::string analysisText = AnalysisTextFor(uriStr, text);
    document::TreePtr savedTree = document::MakeTreePtr(m_parser->Parse(analysisText));

    if (PredefinedStubContributes(uriStr))
    {
        ClaimPredefinedFile(uriStr, true);
        m_predefinedManager.SetDocumentText(uriStr, analysisText);
        ReplaceSymbolsFromTree(uriStr, analysisText, savedTree.get());
    }
    else
    {
        m_symbolTable.ClearDocumentSymbols(uriStr);
        m_predefinedManager.RemoveStub(uriStr);
    }

    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);

    // A save is the point the watcher would have reacted to, had the file not been open.
    const bool wordsChanged = RefreshStubDefinedWords(uriStr, text);

    PublishDiagnostics(uriStr, {});

    if (wordsChanged)
    {
        ReanalyseOpenDocuments();
    }

    AnalyzeConfiguredModules();
}

void Server::ReanalyzeDependentsOnSave(const std::string& uriStr)
{
    const std::string savedPath = CanonicalPathFromUri(uriStr);
    const ModuleClaim savedClaim = !savedPath.empty() ? ClaimFor(savedPath) : ModuleClaim{};
    std::vector<std::string> closureFiles;
    if (!savedPath.empty())
    {
        closureFiles = m_includeGraph.GetModuleClosure(savedPath);
    }
    ankerl::unordered_dense::set<std::string> closureSet(closureFiles.begin(), closureFiles.end());

    for (const auto& [openUri, openText] : m_documentStore.GetSnapshot())
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
            if (m_analysisScheduler && m_analysisScheduler->ShouldDebouncePeer(openUri))
            {
                continue;
            }

            IndexModuleClosure(openUri);
            ScheduleAnalysis(openUri, openText);
        }
    }
}

void Server::HandleNotificationsTextDocument_DidSave(lsp::notifications::TextDocument_DidSave::Params&& params)
{
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    utils::HighResTimer totalTimer;
    std::string uriStr = DocumentKey(params.textDocument.uri.toString());
    LogInfo(fmt::format("[File Save] Processing save and re-analysis for: {}", uriStr));
    m_documentStore.SetClientUri(uriStr, params.textDocument.uri.toString());
    std::string text = params.text.has_value() ? params.text.value() : "";

    if (text.empty())
    {
        if (auto docText = m_documentStore.GetText(uriStr))
        {
            text = std::move(*docText);
        }
    }

    const int version = m_documentStore.GetVersion(uriStr);
    if (m_analysisScheduler)
    {
        m_analysisScheduler->MarkSaved(uriStr, version);
    }

    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);

    if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
    {
        DidSavePredefinedFile(uriStr, text);
        return;
    }

    const std::string analysisText = AnalysisTextFor(uriStr, text);
    document::TreePtr savedTree = document::MakeTreePtr(m_parser->Parse(analysisText));

    if (const std::string savedPath = CanonicalPathFromUri(uriStr); !savedPath.empty())
    {
        m_includeGraph.UpdateFile(utils::WorkspaceIncludeGraph::UpdateFileRequest{
            savedPath, text, *SearchDirectories(), IncludeAllowedRoots(), std::string(ImplicitIncludeExtension())});
    }

    IndexModuleClosure(uriStr);

    bool interfaceChanged = false;
    auto diagnostics = ReplaceSymbolsFromTree(uriStr, analysisText, savedTree.get(), &interfaceChanged);

    double scopeMs = 0.0;
    double checkMs = 0.0;
    analysis::NodeIndex nodeIndex(ts_tree_root_node(savedTree.get()));
    auto semanticDiagnostics = CollectScopesAndAnalyze({.uriStr = uriStr,
                                                        .text = analysisText,
                                                        .tree = savedTree.get(),
                                                        .outScopeMs = &scopeMs,
                                                        .outCheckMs = &checkMs,
                                                        .nodeIndex = &nodeIndex});
    diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

    AppendIncludeDiagnostics(uriStr, analysisText, diagnostics);

    PublishDiagnostics(uriStr, analysisText, diagnostics, version);

    LogInfo(fmt::format("[File Save] Finished save analysis for: {} in {}",
                        uriStr, utils::FormatDuration(totalTimer.ElapsedMs())));

    HandleSavedInterfaceChange(uriStr, interfaceChanged);
}

void Server::HandleSavedInterfaceChange(const std::string& uriStr, bool interfaceChanged)
{
    if (!m_modules.empty())
    {
        WithdrawStaleModuleDiagnostics();
    }

    if (interfaceChanged)
    {
        ReanalyzeDependentsOnSave(uriStr);
    }
    else
    {
        LogInfo(fmt::format("Save {}: public interface unchanged, skipping cascading re-analysis", uriStr));
    }
}

std::string Server::AnalysisTextFor(const std::string& uriStr, const std::string& text) const
{
    if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
    {
        return angel_lsp::utils::SanitizePredefinedContent(text);
    }
    return text;
}

void Server::DidOpenPredefinedFile(const DidOpenPredefinedRequest& req)
{
    const std::string analysisText = AnalysisTextFor(req.uriStr, req.text);
    const bool contributes = PredefinedStubContributes(req.uriStr);

    if (!contributes)
    {
        m_documentStore.OpenDocument(DocumentStore::OpenDocumentRequest{
            req.uriStr, req.text, req.version, document::MakeTreePtr(nullptr), req.clientUri});
        m_symbolTable.ClearDocumentSymbols(req.uriStr);
        m_predefinedManager.RemoveStub(req.uriStr);
        if (m_workspaceStore)
        {
            m_workspaceStore->SetPreindexedDocument(req.uriStr, nullptr);
        }
        const double totalMs = req.totalTimer.ElapsedMs();
        LogInfo(fmt::format("[File Open] Predefined file opened (non-contributing): {} in {}",
                            req.uriStr, utils::FormatDuration(totalMs)));
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
        m_documentStore.LinkDocument(req.uriStr, preindexedDoc, req.clientUri);
        const double totalMs = req.totalTimer.ElapsedMs();
        LogInfo(fmt::format("[File Open] Finished opening predefined file (zero-copy link): {} in {}",
                            req.uriStr, utils::FormatDuration(totalMs)));
        LogInfo(fmt::format("[Predefined Zero-Copy Fast Path] File: {} content unchanged; linked pre-indexed snapshot "
                            "in {}",
                            req.uriStr, utils::FormatDuration(totalMs)));
        LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {} (Parse: 0.00 ms, Collector: 0.00 ms, "
                            "Scopes: 0.00 ms, Checkers: 0.00 ms)",
                            req.uriStr, utils::FormatDuration(totalMs)));
        PublishDiagnostics(req.uriStr, {}, req.version);
        return;
    }

    m_documentStore.OpenDocument(DocumentStore::OpenDocumentRequest{
        req.uriStr, req.text, req.version, document::MakeTreePtr(nullptr), req.clientUri});

    ScheduleAnalysisImmediate(ScheduleAnalysisRequest{
        .uriStr = req.uriStr,
        .text = req.text,
        .force = true,
        .tree = document::MakeTreePtr(nullptr),
        .version = req.version,
    });

    const double totalMs = req.totalTimer.ElapsedMs();
    LogInfo(fmt::format("[File Open] Predefined file offloaded to analysis worker: {} in {}",
                        req.uriStr, utils::FormatDuration(totalMs)));
    LogInfo(fmt::format("[Predefined Offload Fast Path] File: {} reparse offloaded to worker in {}",
                        req.uriStr, utils::FormatDuration(totalMs)));
}

void Server::HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params&& params)
{
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    utils::HighResTimer totalTimer;
    std::string uriStr = DocumentKey(params.textDocument.uri.toString());
    const std::string clientUri = params.textDocument.uri.toString();
    const int version = params.textDocument.version;
    std::string text = std::move(params.textDocument.text);
    LogInfo(fmt::format("[File Open] Opening document: {}", uriStr));

    if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
    {
        DidOpenPredefinedFile(DidOpenPredefinedRequest{uriStr, text, version, clientUri, totalTimer});
        return;
    }

    const std::string analysisText = AnalysisTextFor(uriStr, text);

    utils::HighResTimer parseTimer;
    TSTree* tree = m_parser->Parse(analysisText);
    double parseMs = parseTimer.ElapsedMs();
    m_documentStore.OpenDocument(
        DocumentStore::OpenDocumentRequest{uriStr, text, version, document::MakeTreePtr(tree), clientUri});

    if (m_config.features.enablePredefinedLoader && !IsPredefinedReady())
    {
        WaitForPredefinedReady(std::chrono::milliseconds(5000));
    }

    utils::HighResTimer colTimer;
    auto diagnostics = ReplaceSymbolsFromTree(uriStr, analysisText, tree);
    double colMs = colTimer.ElapsedMs();

    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);

    // Before analysis, not after: the module the file belongs to supplies declarations this
    // file legitimately uses, and without them every one of them would be reported undeclared.
    IndexModuleClosure(uriStr);

    double scopeMs = 0.0;
    double checkMs = 0.0;
    analysis::NodeIndex nodeIndex(ts_tree_root_node(tree));
    auto semanticDiagnostics = CollectScopesAndAnalyze({.uriStr = uriStr,
                                                        .text = analysisText,
                                                        .tree = tree,
                                                        .outScopeMs = &scopeMs,
                                                        .outCheckMs = &checkMs,
                                                        .nodeIndex = &nodeIndex});
    diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

    AppendIncludeDiagnostics(uriStr, analysisText, diagnostics);

    if (m_config.features.enablePredefinedLoader && !IsPredefinedReady())
    {
        LogInfo(fmt::format("Predefined stubs still indexing; deferring initial diagnostics for {}", uriStr));
    }
    else
    {
        PublishDiagnostics(uriStr, analysisText, diagnostics, version);
    }

    const double totalMs = totalTimer.ElapsedMs();
    LogInfo(fmt::format("[File Open] Finished opening document: {} in {}",
                        uriStr, utils::FormatDuration(totalMs)));
    LogAnalysisProfile("[File Open]", uriStr, {totalMs, parseMs, colMs, scopeMs, checkMs});
}

void Server::ApplyContentChange(std::string& buffer, document::TreePtr& workingTree,
                                const lsp::TextDocumentContentChangeEvent& change, bool isPredefined)
{
    if (std::holds_alternative<lsp::TextDocumentContentChangePartial>(change))
    {
        const auto& rt = std::get<lsp::TextDocumentContentChangePartial>(change);

        if (workingTree && !isPredefined)
        {
            TSInputEdit edit = ComputeTsInputEdit(buffer, rt, m_positionEncoding);
            ts_tree_edit(workingTree.get(), &edit);
        }

        angel_lsp::utils::ApplyIncrementalChange(
            buffer,
            angel_lsp::utils::TextChangeRange{rt.range.start.line, rt.range.start.character, rt.range.end.line,
                                              rt.range.end.character},
            rt.text, m_positionEncoding);
    }
    else if (std::holds_alternative<lsp::TextDocumentContentChangeWholeDocument>(change))
    {
        const auto& t = std::get<lsp::TextDocumentContentChangeWholeDocument>(change);
        buffer = t.text;
        workingTree.reset();
    }
}

void Server::HandleNotificationsTextDocument_DidChange(lsp::notifications::TextDocument_DidChange::Params&& params)
{
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);

    std::string uriStr = DocumentKey(params.textDocument.uri.toString());
    m_documentStore.SetClientUri(uriStr, params.textDocument.uri.toString());

    auto doc = m_documentStore.GetDocument(uriStr);
    if (!doc)
    {
        return;
    }

    const int version = params.textDocument.version;
    std::string buffer = doc->text;

    const bool isPredefined = angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension);
    document::TreePtr workingTree =
        document::MakeTreePtr((doc->tree && !isPredefined) ? ts_tree_copy(doc->tree.get()) : nullptr);

    for (const auto& change : params.contentChanges)
    {
        ApplyContentChange(buffer, workingTree, change, isPredefined);
    }

    const std::string analysisText = AnalysisTextFor(uriStr, buffer);

    utils::HighResTimer parseTimer;
    // For a predefined stub, reparse cleanly without reusing incremental state.
    TSTree* oldTree = (isPredefined || !workingTree) ? nullptr : workingTree.get();
    TSTree* newTree = m_parser->Parse(analysisText, oldTree);
    m_documentStore.UpdateDocument(uriStr, buffer, version, document::MakeTreePtr(newTree));
    double parseMs = parseTimer.ElapsedMs();
    LogInfo(fmt::format("[DidChange Incremental Parse] File: {} | Parse: {:.2f} ms", uriStr, parseMs));

    if (isPredefined)
    {
        m_predefinedManager.SetDocumentText(uriStr, analysisText);
    }

    // The reparse above is incremental and cheap, and stays on this thread so a request
    ScheduleAnalysis(ScheduleAnalysisRequest{
        .uriStr = uriStr,
        .text = buffer,
        .force = false,
        .tree = angel_lsp::document::MakeTreePtr(newTree ? ts_tree_copy(newTree) : nullptr),
        .version = version,
        .generation = 0,
    });
}

bool Server::RestoreClosedModuleFile(const std::string& uriStr, const std::string& path)
{
    if (path.empty())
    {
        return false;
    }

    if (const auto indexed = m_indexedUriByPath.find(path);
        indexed != m_indexedUriByPath.end() && indexed->second == uriStr)
    {
        m_indexedUriByPath.erase(indexed);
    }

    bool isClaimedOrPublished = false;
    {
        std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
        isClaimedOrPublished = (ClaimFor(path).owner != nullptr || m_publishedForModules.contains(uriStr));
    }
    if (!isClaimedOrPublished)
    {
        return false;
    }

    angel_lsp::parser::AngelScriptParser restoreParser(m_logger.get());
    IndexClosureFile(path, restoreParser);

    LogInfo(fmt::format("[File Read] Reading closed module file from disk: {}", path));
    utils::HighResTimer readTimer;
    std::ifstream file(path, std::ios::binary);
    if (file.is_open())
    {
        std::ostringstream ss;
        ss << file.rdbuf();
        const std::string content = ss.str();
        LogInfo(fmt::format("[File Read] Finished reading file: {} in {} ({} bytes)",
                            path, utils::FormatDuration(readTimer.ElapsedMs()), content.size()));
        {
            std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
            m_publishedForModules.insert(uriStr);
        }
        ScheduleAnalysis(uriStr, content, true);
    }
    return true;
}

void Server::HandleNotificationsTextDocument_DidClose(lsp::notifications::TextDocument_DidClose::Params&& params)
{
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    std::string uriStr = DocumentKey(params.textDocument.uri.toString());
    LogInfo(fmt::format("[File Close] Document closed: {}", uriStr));
    m_documentStore.CloseDocument(uriStr);

    if (m_analysisScheduler)
    {
        m_analysisScheduler->Cancel(uriStr);
    }

    // The cached token payload is only meaningful while the client still holds it.
    {
        std::lock_guard<std::mutex> lock(m_semanticTokensMutex);
        m_semanticTokensCache.erase(uriStr);
    }

    if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
    {
        PublishDiagnostics(uriStr, {});
        return;
    }

    ReleaseModuleClosure(uriStr);

    m_symbolTable.ClearDocumentSymbols(uriStr);
    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);
    m_closureDocuments.erase(uriStr);

    const std::string path = CanonicalPathFromUri(uriStr);
    const bool isModuleFile = RestoreClosedModuleFile(uriStr, path);

    // Closing a file does not remove it from the modules of the documents still open.
    const std::vector<std::string> stillOpen = m_documentStore.GetOpenUris();

    for (const auto& openUri : stillOpen)
    {
        if (!angel_lsp::utils::IsPredefinedFile(openUri, m_config.info.predefinedFileExtension))
        {
            const size_t newlyIndexed = IndexModuleClosure(openUri);
            if (newlyIndexed > 0)
            {
                if (auto openText = m_documentStore.GetText(openUri))
                {
                    ScheduleAnalysis(openUri, *openText);
                }
            }
        }
    }

    if (!isModuleFile)
    {
        PublishDiagnostics(uriStr, {});
    }
}
} // namespace angel_lsp
