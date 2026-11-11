#include "analysis/index/NodeIndex.h"
#include "lsp/Server.h"
#include "parser/SyntaxTree.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <fstream>
#include <spdlog/fmt/fmt.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{
static parser::TextChangeEdit ComputeTextChangeEdit(const std::string& buffer,
                                                    const lsp::TextDocumentContentChangePartial& rt,
                                                    angel_lsp::utils::PositionEncoding encoding)
{
    uint32_t startLine = rt.range.start.line;
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

    uint32_t newEndRow = 0;
    uint32_t newEndColumn = 0;
    if (newlineCount > 0)
    {
        newEndRow = startLine + newlineCount;
        newEndColumn = static_cast<uint32_t>(rt.text.size() - (lastNewlinePos + 1));
    }
    else
    {
        newEndRow = startLine;
        newEndColumn = static_cast<uint32_t>(startChar + rt.text.size());
    }

    parser::TextChangeEdit edit;
    edit.startByte = start_byte;
    edit.oldEndByte = old_end_byte;
    edit.newEndByte = new_end_byte;
    edit.startRow = startLine;
    edit.startColumn = startChar;
    edit.oldEndRow = endLine;
    edit.oldEndColumn = endChar;
    edit.newEndRow = newEndRow;
    edit.newEndColumn = newEndColumn;
    return edit;
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
            parser::TextChangeEdit edit = ComputeTextChangeEdit(buffer, rt, encoding);
            parser::ApplyTreeEdit(workingTree, edit);
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
    const bool canReuseTree = prepared.text == buffer;
    auto newTree = m_parser->ParseTree(prepared.text, canReuseTree ? workingTree : nullptr);
    if (buffer != doc->text)
    {
        m_scopeIndex.InvalidateDocumentOccurrences(uriStr);
    }
    GetDocumentStore().UpdateDocument(uriStr, buffer, version, std::move(newTree),
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
