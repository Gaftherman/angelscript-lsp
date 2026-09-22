#include "lsp/ModuleIndex.h"

namespace angel_lsp
{
void ModuleIndex::UpdateFile(const UpdateFileRequest& request)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_includeGraph.UpdateFile(utils::WorkspaceIncludeGraph::UpdateFileRequest{
        request.filePath, request.content, request.searchDirectories, request.allowedRoots, request.implicitExtension});
}

void ModuleIndex::UpdateFile(const std::string& filePath, std::string_view content,
                             const std::vector<std::string>& searchDirectories)
{
    UpdateFile(UpdateFileRequest{filePath, content, searchDirectories, {}, {}});
}

void ModuleIndex::RemoveFile(const std::string& filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_includeGraph.RemoveFile(filePath);
}

std::vector<std::string> ModuleIndex::GetModuleClosure(const std::string& filePath) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_includeGraph.GetModuleClosure(filePath);
}

void ModuleIndex::AssociateClosure(const std::string& openUri, std::vector<std::string> closureUris)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_openDocumentClosures[openUri] = std::move(closureUris);
}

std::vector<std::string> ModuleIndex::ReleaseClosure(const std::string& openUri)
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

void ModuleIndex::SetClosureDocument(const std::string& uri, std::string content)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_closureDocuments[uri] = std::move(content);
}

bool ModuleIndex::HasClosureDocument(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_closureDocuments.contains(uri);
}

std::optional<std::string> ModuleIndex::GetClosureDocument(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_closureDocuments.find(uri); it != m_closureDocuments.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void ModuleIndex::PurgeClosureDocument(const std::string& uri)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_closureDocuments.erase(uri);
}

void ModuleIndex::SetIndexedUri(const std::string& path, const std::string& uri)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_indexedUriByPath[path] = uri;
}

std::optional<std::string> ModuleIndex::GetIndexedUri(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_indexedUriByPath.find(path); it != m_indexedUriByPath.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void ModuleIndex::RemoveIndexedPath(const std::string& path)
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
    auto it = std::lower_bound(m_exportedSymbols.begin(), m_exportedSymbols.end(), prefix,
                               [](const ExportedSymbol& sym, std::string_view p) { return sym.name < p; });
    std::vector<ExportedSymbol> results;
    while (it != m_exportedSymbols.end() && it->name.starts_with(prefix))
    {
        results.push_back(*it);
        ++it;
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
