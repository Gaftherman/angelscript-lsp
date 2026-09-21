#include "analysis/NodeIndex.h"
#include "lsp/PositionCodec.h"
#include "lsp/Server.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include <fstream>
#include <iterator>
#include <spdlog/fmt/fmt.h>
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
    if (savedTree)
    {
        m_scopeIndex.SetScopeTree(
            uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(savedTree.get()), analysisText));
        m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(savedTree.get()), analysisText));
    }

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
    std::string uriStr = DocumentKey(params.textDocument.uri.toString());
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

    auto existingText = m_predefinedManager.GetDocumentText(req.uriStr);
    if (existingText && *existingText == analysisText && contributes)
    {
        PublishDiagnostics(req.uriStr, {}, req.version);
        const double totalMs = req.totalTimer.ElapsedMs();
        LogInfo(
            fmt::format("[Predefined Fast Path] File: {} content unchanged; bypassed re-indexing. Elapsed: {:.2f} ms",
                        req.uriStr, totalMs));
        LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: "
                            "{:.2f} ms, Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
                            req.uriStr, totalMs, req.parseMs, 0.0, 0.0, 0.0));
        return;
    }

    utils::HighResTimer colTimer;
    if (contributes)
    {
        ClaimPredefinedFile(req.uriStr, true);
        m_predefinedManager.SetDocumentText(req.uriStr, analysisText);
        ReplaceSymbolsFromTree(req.uriStr, analysisText, req.tree);
    }
    else
    {
        m_symbolTable.ClearDocumentSymbols(req.uriStr);
        m_predefinedManager.RemoveStub(req.uriStr);
    }
    const double colMs = colTimer.ElapsedMs();

    utils::HighResTimer scopeTimer;
    m_scopeIndex.ClearDocument(req.uriStr);
    m_callGraph.ClearDocument(req.uriStr);
    if (req.tree)
    {
        m_scopeIndex.SetScopeTree(
            req.uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(req.tree), analysisText));
        m_callGraph.SetDocumentCalls(req.uriStr, analysis::CollectCalls(ts_tree_root_node(req.tree), analysisText));
    }
    const double scopeMs = scopeTimer.ElapsedMs();

    PublishDiagnostics(req.uriStr, {}, req.version);

    const double totalMs = req.totalTimer.ElapsedMs();
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: {:.2f} ms, "
                        "Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
                        req.uriStr, totalMs, req.parseMs, colMs, scopeMs, 0.0));

    const bool wordsChanged = RefreshStubDefinedWords(req.uriStr, req.text);
    if (wordsChanged)
    {
        ReanalyseOpenDocuments();
    }
}

void Server::HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params&& params)
{
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    utils::HighResTimer totalTimer;
    std::string uriStr = DocumentKey(params.textDocument.uri.toString());
    const std::string clientUri = params.textDocument.uri.toString();
    const int version = params.textDocument.version;
    std::string text = std::move(params.textDocument.text);

    const std::string analysisText = AnalysisTextFor(uriStr, text);

    utils::HighResTimer parseTimer;
    TSTree* tree = m_parser->Parse(analysisText);
    double parseMs = parseTimer.ElapsedMs();
    m_documentStore.OpenDocument(
        DocumentStore::OpenDocumentRequest{uriStr, text, version, document::MakeTreePtr(tree), clientUri});

    if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
    {
        DidOpenPredefinedFile(DidOpenPredefinedRequest{uriStr, text, version, tree, parseMs, totalTimer});
        return;
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

    PublishDiagnostics(uriStr, analysisText, diagnostics, version);

    double totalMs = totalTimer.ElapsedMs();
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: {:.2f} ms, "
                        "Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
                        uriStr, totalMs, parseMs, colMs, scopeMs, checkMs));
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

    std::ifstream file(path, std::ios::binary);
    if (file.is_open())
    {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
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

    const std::string path = CanonicalPathFromUri(uriStr);
    const bool isModuleFile = RestoreClosedModuleFile(uriStr, path);

    // Closing a file does not remove it from the modules of the documents still open.
    const std::vector<std::string> stillOpen = m_documentStore.GetOpenUris();

    for (const auto& openUri : stillOpen)
    {
        if (!angel_lsp::utils::IsPredefinedFile(openUri, m_config.info.predefinedFileExtension))
        {
            IndexModuleClosure(openUri);
        }
    }

    if (!isModuleFile)
    {
        PublishDiagnostics(uriStr, {});
    }
}
} // namespace angel_lsp
