#include "lsp/state/ModuleIndex.h"

#include "utils/Utils.h"

#include <algorithm>

namespace angel_lsp
{
void ModuleIndex::UpdateFile(const UpdateFileRequest& request)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_includeGraph.UpdateFile(utils::WorkspaceIncludeGraph::UpdateFileRequest{
        request.filePath, request.content, request.searchDirectories, request.allowedRoots, request.implicitExtension});
}

void ModuleIndex::UpdateFile(std::string_view filePath, std::string_view content,
                             std::span<const std::string> searchDirectories)
{
    UpdateFile(UpdateFileRequest{std::string(filePath),
                                 content,
                                 std::vector<std::string>(searchDirectories.begin(), searchDirectories.end()),
                                 {},
                                 {}});
}

void ModuleIndex::RemoveFile(std::string_view filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_includeGraph.RemoveFile(std::string(filePath));
}

std::vector<std::string> ModuleIndex::GetModuleClosure(std::string_view filePath) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_includeGraph.GetModuleClosure(filePath);
}

void ModuleIndex::AssociateClosure(std::string_view openUri, std::vector<std::string> closureUris)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_openDocumentClosures.insert_or_assign(openUri, std::move(closureUris));
}

std::vector<std::string> ModuleIndex::ReleaseClosure(std::string_view openUri)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_openDocumentClosures.find(openUri);
    if (it == m_openDocumentClosures.end())
    {
        return {};
    }

    std::vector<std::string> released = std::move(it->second);
    m_openDocumentClosures.erase(it);

    std::vector<std::string> unreferenced;
    for (const auto& uriStr : released)
    {
        bool stillNeeded = false;
        for (const auto& [otherUri, otherClosure] : m_openDocumentClosures)
        {
            if (std::find(otherClosure.begin(), otherClosure.end(), uriStr) != otherClosure.end())
            {
                stillNeeded = true;
                break;
            }
        }

        if (!stillNeeded)
        {
            m_closureDocuments.erase(uriStr);
            unreferenced.push_back(uriStr);
        }
    }

    return unreferenced;
}

void ModuleIndex::SetClosureDocument(std::string_view uri, std::string content)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_closureDocuments.insert_or_assign(uri, std::move(content));
}

bool ModuleIndex::HasClosureDocument(std::string_view uri) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closureDocuments.contains(uri);
}

std::optional<std::string> ModuleIndex::GetClosureDocument(std::string_view uri) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_closureDocuments.find(uri); it != m_closureDocuments.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void ModuleIndex::PurgeClosureDocument(std::string_view uri)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_closureDocuments.erase(uri);
}

void ModuleIndex::SetIndexedUri(std::string_view path, std::string_view uri)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_indexedUriByPath.insert_or_assign(path, std::string(uri));
}

std::optional<std::string> ModuleIndex::GetIndexedUri(std::string_view path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_indexedUriByPath.find(path); it != m_indexedUriByPath.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void ModuleIndex::RemoveIndexedPath(std::string_view path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_indexedUriByPath.erase(path);
}

void ModuleIndex::AddExportedSymbol(ExportedSymbol symbol)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::lower_bound(m_exportedSymbols.begin(), m_exportedSymbols.end(), symbol.name,
                               [](const ExportedSymbol& sym, std::string_view prefix) { return sym.name < prefix; });
    m_exportedSymbols.insert(it, std::move(symbol));
}

void ModuleIndex::SetExportedSymbols(std::vector<ExportedSymbol> symbols)
{
    std::sort(symbols.begin(), symbols.end(),
              [](const ExportedSymbol& a, const ExportedSymbol& b) { return a.name < b.name; });
    std::lock_guard<std::mutex> lock(m_mutex);
    m_exportedSymbols = std::move(symbols);
}

std::vector<ModuleIndex::ExportedSymbol> ModuleIndex::FindSymbolsByPrefix(std::string_view prefix) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (prefix.empty())
    {
        return m_exportedSymbols;
    }
    std::vector<ExportedSymbol> results;
    char c = prefix[0];
    char cUpper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    char cLower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto scanChar = [&](char firstChar)
    {
        std::string startPattern(1, firstChar);
        auto it = std::lower_bound(m_exportedSymbols.begin(), m_exportedSymbols.end(), startPattern,
                                   [](const ExportedSymbol& sym, std::string_view p) { return sym.name < p; });
        for (; it != m_exportedSymbols.end(); ++it)
        {
            if (it->name.empty() || it->name[0] != firstChar)
            {
                break;
            }
            if (angel_lsp::utils::CaseInsensitiveStartsWith(it->name, prefix))
            {
                results.push_back(*it);
            }
        }
    };

    scanChar(cUpper);
    if (cLower != cUpper)
    {
        scanChar(cLower);
    }
    return results;
}

void ModuleIndex::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_includeGraph.Clear();
    m_closureDocuments.clear();
    m_openDocumentClosures.clear();
    m_indexedUriByPath.clear();
    m_exportedSymbols.clear();
}
} // namespace angel_lsp
