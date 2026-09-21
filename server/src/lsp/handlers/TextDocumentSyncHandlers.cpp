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
void Server::HandleNotificationsTextDocument_WillSave(
    [[maybe_unused]] lsp::notifications::TextDocument_WillSave::Params&& params)
{
    // The server has nothing it must do before a save - the analysis is already current and the
    // document text is already held - so the handler body is empty apart from this comment: it exists
    // so the notification is consumed rather than dropped by a server that advertised the capability.
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
        const std::string analysisText = AnalysisTextFor(uriStr, text);
        document::TreePtr savedTree = document::MakeTreePtr(m_parser->Parse(analysisText));
        std::vector<angel_lsp::analysis::Diagnostic> diagnostics;

        if (PredefinedStubContributes(uriStr))
        {
            ClaimPredefinedFile(uriStr, true);
            m_predefinedManager.SetDocumentText(uriStr, analysisText);
            diagnostics = ReplaceSymbolsFromTree(uriStr, analysisText, savedTree.get());
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
            m_callGraph.SetDocumentCalls(uriStr,
                                         analysis::CollectCalls(ts_tree_root_node(savedTree.get()), analysisText));
        }

        // A save is the point the watcher would have reacted to, had the file not been open -
        // didChangeWatchedFiles skips open documents on purpose, so this is the only place a
        // saved stub can announce that its `#define`s moved.
        const bool wordsChanged = RefreshStubDefinedWords(uriStr, text);

        PublishDiagnostics(uriStr, {});

        if (wordsChanged)
        {
            ReanalyseOpenDocuments();
        }

        AnalyzeConfiguredModules();
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
    auto semanticDiagnostics =
        CollectScopesAndAnalyze(uriStr, analysisText, savedTree.get(), &scopeMs, &checkMs, &nodeIndex);
    diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

    AppendIncludeDiagnostics(uriStr, analysisText, diagnostics);

    PublishDiagnostics(uriStr, analysisText, diagnostics, version);

    if (!m_modules.empty())
    {
        WithdrawStaleModuleDiagnostics();
    }

    if (interfaceChanged)
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

void Server::HandleNotificationsTextDocument_DidOpen(lsp::notifications::TextDocument_DidOpen::Params&& params)
{
    std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
    utils::HighResTimer totalTimer;
    std::string uriStr = DocumentKey(params.textDocument.uri.toString());
    const std::string clientUri = params.textDocument.uri.toString();
    const int version = params.textDocument.version;
    std::string text = params.textDocument.text;

    const std::string analysisText = AnalysisTextFor(uriStr, text);

    utils::HighResTimer parseTimer;
    TSTree* tree = m_parser->Parse(analysisText);
    double parseMs = parseTimer.ElapsedMs();
    m_documentStore.OpenDocument(
        DocumentStore::OpenDocumentRequest{uriStr, text, version, document::MakeTreePtr(tree), clientUri});

    if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
    {
        const bool contributes = PredefinedStubContributes(uriStr);

        bool contentUnchanged = false;
        auto existingText = m_predefinedManager.GetDocumentText(uriStr);
        if (existingText && *existingText == analysisText)
        {
            contentUnchanged = true;
        }

        if (contentUnchanged && contributes)
        {
            PublishDiagnostics(uriStr, {}, version);
            double totalMs = totalTimer.ElapsedMs();
            LogInfo(fmt::format(
                "[Predefined Fast Path] File: {} content unchanged; bypassed re-indexing. Elapsed: {:.2f} ms", uriStr,
                totalMs));
            LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: "
                                "{:.2f} ms, Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
                                uriStr, totalMs, parseMs, 0.0, 0.0, 0.0));
            return;
        }

        utils::HighResTimer colTimer;
        if (contributes)
        {
            ClaimPredefinedFile(uriStr, true);
            m_predefinedManager.SetDocumentText(uriStr, analysisText);
            ReplaceSymbolsFromTree(uriStr, analysisText, tree);
        }
        else
        {
            m_symbolTable.ClearDocumentSymbols(uriStr);
            m_predefinedManager.RemoveStub(uriStr);
        }
        double colMs = colTimer.ElapsedMs();

        utils::HighResTimer scopeTimer;
        m_scopeIndex.ClearDocument(uriStr);
        m_callGraph.ClearDocument(uriStr);
        if (tree)
        {
            m_scopeIndex.SetScopeTree(
                uriStr, m_localScopeCollector->CollectScopesFromTree(ts_tree_root_node(tree), analysisText));
            m_callGraph.SetDocumentCalls(uriStr, analysis::CollectCalls(ts_tree_root_node(tree), analysisText));
        }
        double scopeMs = scopeTimer.ElapsedMs();
        double checkMs = 0.0;

        PublishDiagnostics(uriStr, {}, version);

        const bool wordsChanged = RefreshStubDefinedWords(uriStr, text);
        if (wordsChanged)
        {
            ReanalyseOpenDocuments();
        }

        double totalMs = totalTimer.ElapsedMs();
        LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: {:.2f} "
                            "ms, Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
                            uriStr, totalMs, parseMs, colMs, scopeMs, checkMs));

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
    auto semanticDiagnostics = CollectScopesAndAnalyze(uriStr, analysisText, tree, &scopeMs, &checkMs, &nodeIndex);
    diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());

    AppendIncludeDiagnostics(uriStr, analysisText, diagnostics);

    PublishDiagnostics(uriStr, analysisText, diagnostics, version);

    double totalMs = totalTimer.ElapsedMs();
    LogInfo(fmt::format("[Open/Change Profile] File: {} | Total: {:.2f} ms (Parse: {:.2f} ms, Collector: {:.2f} ms, "
                        "Scopes: {:.2f} ms, Checkers: {:.2f} ms)",
                        uriStr, totalMs, parseMs, colMs, scopeMs, checkMs));
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
        if (std::holds_alternative<lsp::TextDocumentContentChangePartial>(change))
        {
            const auto& rt = std::get<lsp::TextDocumentContentChangePartial>(change);

            if (workingTree && !isPredefined)
            {
                uint32_t startLine = rt.range.start.line;
                // rt.range is expressed in the negotiated encoding, while TSPoint::column and every
                // byte offset below are byte columns. Converting here is what keeps the server's
                // buffer in step with the editor on documents containing non-ASCII text: an edit
                // applied at the wrong offset desynchronises the two permanently.
                uint32_t startChar = angel_lsp::utils::LspCharToByteColumn(
                    angel_lsp::utils::GetLine(buffer, startLine), rt.range.start.character, m_positionEncoding);
                uint32_t endLine = rt.range.end.line;
                uint32_t endChar = angel_lsp::utils::LspCharToByteColumn(angel_lsp::utils::GetLine(buffer, endLine),
                                                                         rt.range.end.character, m_positionEncoding);

                uint32_t start_byte = static_cast<uint32_t>(angel_lsp::utils::PositionToOffset(
                    buffer, startLine, rt.range.start.character, m_positionEncoding));
                uint32_t old_end_byte = static_cast<uint32_t>(
                    angel_lsp::utils::PositionToOffset(buffer, endLine, rt.range.end.character, m_positionEncoding));
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
    // arriving right after the edit is answered against a current tree. Symbol collection,
    // scope building and semantic analysis rebuild whole-document state instead - that is what
    // makes a 3000-line file feel slow when it runs on every keystroke - so they are queued and
    // run once typing pauses. Until then the symbol table still holds the previous revision,
    // which is the same trade every other language server makes.
    ScheduleAnalysis(uriStr, buffer, false, newTree ? ts_tree_copy(newTree) : nullptr, version);
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

    // The cached token payload is only meaningful while the client still holds it. Dropping it
    // here also means a reopened document starts from a full stream rather than a delta against
    // a payload the client threw away when it closed the editor tab.
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

    bool isModuleFile = false;
    if (const std::string path = CanonicalPathFromUri(uriStr); !path.empty())
    {
        if (const auto indexed = m_indexedUriByPath.find(path);
            indexed != m_indexedUriByPath.end() && indexed->second == uriStr)
            m_indexedUriByPath.erase(indexed);

        // If the document that was closed belongs to a configured module, revert it to an
        // on-disk closure file so its declarations remain visible to the module.
        bool isClaimedOrPublished = false;
        {
            std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
            isClaimedOrPublished = (ClaimFor(path).owner != nullptr || m_publishedForModules.contains(uriStr));
        }
        if (isClaimedOrPublished)
        {
            isModuleFile = true;
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
        }
    }

    // Closing a file does not remove it from the modules of the documents still open. Re-running
    // their closures picks it back up as an on-disk closure file - and costs nothing for the
    // files already indexed, which are skipped by URI.
    const std::vector<std::string> stillOpen = m_documentStore.GetOpenUris();

    for (const auto& openUri : stillOpen)
    {
        if (!angel_lsp::utils::IsPredefinedFile(openUri, m_config.info.predefinedFileExtension))
            IndexModuleClosure(openUri);
    }

    if (!isModuleFile)
    {
        PublishDiagnostics(uriStr, {});
    }
}
} // namespace angel_lsp
