#include "analysis/index/NodeIndex.h"
#include "analysis/types/EngineConfigConverter.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "parser/SyntaxTree.h"
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
static TSInputEdit ComputeTsInputEdit(const std::string& buffer, const lsp::TextDocumentContentChangePartial& rt,
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

void Server::HandleNotificationsTextDocument_WillSave(
    [[maybe_unused]] lsp::notifications::TextDocument_WillSave::Params&& params)
{
    // The server has nothing it must do before a save - the analysis is already current and the
    // document text is already held - so the handler body is empty apart from this comment.
}

void Server::DidSavePredefinedFile(const std::string& uriStr, const std::string& text)
{
    const std::string analysisText = PrepareAnalysisSource(uriStr, text).text;
    parser::TreePtr savedTree = parser::MakeTreePtr(m_parser->Parse(analysisText));

    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        m_closureDocuments[uriStr] = {std::make_shared<const std::string>(text), ++m_nextClosedRevision};
    }

    if (PredefinedStubContributes(uriStr))
    {
        ClaimPredefinedFile(uriStr, true);
        m_predefinedManager.SetDocumentText(uriStr, analysisText);
        ReplaceSymbolsFromTree(uriStr, analysisText, &savedTree);
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
    if (!wordsChanged)
    {
        AnalyzeConfiguredModules();
    }
}

void Server::ReanalyzeDependentsOnSave(const std::string& uriStr)
{
    const std::string savedPath = CanonicalPathFromUri(uriStr);
    const ModuleClaim savedClaim = !savedPath.empty() ? ClaimFor(savedPath) : ModuleClaim{};
    std::vector<std::string> closureFiles;
    if (!savedPath.empty())
    {
        closureFiles = GetIncludeGraph().GetModuleClosure(savedPath);
    }
    ankerl::unordered_dense::set<std::string> closureSet(closureFiles.begin(), closureFiles.end());

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
    auto diagnostics =
        ReplaceSymbolsFromTree(uriStr, prepared.text, &syntax.tree, &interfaceChanged, &syntax.index);

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

void Server::UpdateSavedDocumentCaches(const std::string& uriStr, const std::string& text,
                                       const std::vector<utils::ExcludedLineRange>& excluded)
{
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        m_closureDocuments[uriStr] = {std::make_shared<const std::string>(text), ++m_nextClosedRevision};
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

void Server::HandleSavedInterfaceChange(const std::string& uriStr, bool interfaceChanged)
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
    GetDocumentStore().OpenDocument(DocumentStore::OpenDocumentRequest{req.uriStr, req.text, req.version,
                                                                       std::move(tree), req.clientUri});

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

static void ApplyContentChange(std::string& buffer, parser::TreePtr& workingTree,
                               const lsp::TextDocumentContentChangeEvent& change, bool isPredefined,
                               utils::PositionEncoding encoding)
{
    if (std::holds_alternative<lsp::TextDocumentContentChangePartial>(change))
    {
        const auto& rt = std::get<lsp::TextDocumentContentChangePartial>(change);

        if (workingTree && !isPredefined)
        {
            TSInputEdit edit = ComputeTsInputEdit(buffer, rt, encoding);
            ts_tree_edit(workingTree.get(), &edit);
        }

        angel_lsp::utils::ApplyIncrementalChange(
            buffer,
            angel_lsp::utils::TextChangeRange{rt.range.start.line, rt.range.start.character, rt.range.end.line,
                                              rt.range.end.character},
            rt.text, encoding);
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
    GetDocumentStore().SetClientUri(uriStr, params.textDocument.uri.toString());

    auto doc = GetDocumentStore().GetDocument(uriStr);
    if (!doc)
    {
        return;
    }

    const int version = params.textDocument.version;
    std::string buffer = doc->text;

    const bool isPredefined = angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension);
    const bool canEditTree = doc->tree && !isPredefined && doc->ParsedSource() == doc->text;
    auto workingTree = parser::CopyTree(canEditTree ? doc->tree.get() : nullptr);

    for (const auto& change : params.contentChanges)
    {
        ApplyContentChange(buffer, workingTree, change, isPredefined, GetPositionEncoding());
    }

    auto prepared = PrepareAnalysisSource(uriStr, buffer);

    utils::HighResTimer parseTimer;
    // Preprocessing may change other lines that the editor's incremental edits did not cover.
    TSTree* oldTree = prepared.text == buffer ? workingTree.get() : nullptr;
    TSTree* newTree = m_parser->Parse(prepared.text, oldTree);
    if (buffer != doc->text)
    {
        m_scopeIndex.InvalidateDocumentOccurrences(uriStr);
    }
    GetDocumentStore().UpdateDocument(uriStr, buffer, version, parser::MakeTreePtr(newTree),
                                      TransformedSource(buffer, prepared.text));
    double parseMs = parseTimer.ElapsedMs();
    LogInfo(fmt::format("[DidChange Incremental Parse] File: {} | Parse: {:.2f} ms", uriStr, parseMs));

    if (isPredefined)
    {
        m_predefinedManager.SetDocumentText(uriStr, prepared.text);
    }

    ScheduleAnalysis(ScheduleAnalysisRequest{
        .uriStr = uriStr,
        .text = std::move(buffer),
        .version = version,
        .generation = doc->generation,
        .preparedSource = std::move(prepared),
    });
}

bool Server::RestoreClosedModuleFile(const std::string& uriStr, const std::string& path)
{
    if (path.empty())
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        if (const auto indexed = m_indexedUriByPath.find(path);
            indexed != m_indexedUriByPath.end() && indexed->second == uriStr)
        {
            m_indexedUriByPath.erase(indexed);
        }
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
        LogInfo(fmt::format("[File Read] Finished reading file: {} in {} ({} bytes)", path,
                            utils::FormatDuration(readTimer.ElapsedMs()), content.size()));
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
    GetDocumentStore().CloseDocument(uriStr);
    m_protocolSyntax.Forget(uriStr);

    m_diagnosticsPipeline->Cancel(uriStr);

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
    ClearMixinCaches();

    m_symbolTable.ClearDocumentSymbols(uriStr);
    m_scopeIndex.ClearDocument(uriStr);
    m_callGraph.ClearDocument(uriStr);
    WithdrawClosedSnapshot(uriStr);

    const std::string path = CanonicalPathFromUri(uriStr);
    const bool isOwnedModule = (!m_config.modules.empty() && ClaimFor(path).owner != nullptr);
    const bool isModuleFile = isOwnedModule && RestoreClosedModuleFile(uriStr, path);

    // Closing a file does not remove it from the modules of the documents still open.
    const std::vector<std::string> stillOpen = GetDocumentStore().GetOpenUris();

    for (const auto& openUri : stillOpen)
    {
        if (!angel_lsp::utils::IsPredefinedFile(openUri, m_config.info.predefinedFileExtension))
        {
            const size_t newlyIndexed = IndexModuleClosure(openUri);
            if (newlyIndexed > 0)
            {
                if (auto openText = GetDocumentStore().GetText(openUri))
                {
                    ScheduleAnalysis(openUri, *openText);
                }
            }
        }
    }

    if (!isModuleFile)
    {
        {
            std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
            m_publishedForModules.erase(uriStr);
        }
        m_diagnosticsPipeline->InvalidateDiagnostics(uriStr);
        PublishDiagnostics(uriStr, {});
    }
}
} // namespace angel_lsp
