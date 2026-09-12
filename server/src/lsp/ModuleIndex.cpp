#include "lsp/ModuleIndex.h"

namespace angel_lsp
{
    void ModuleIndex::UpdateFile(const std::string &filePath, std::string_view content,
                                 const std::vector<std::string> &searchDirectories,
                                 const std::vector<std::string> &allowedRoots,
                                 std::string_view implicitExtension)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_includeGraph.UpdateFile(filePath, content, searchDirectories, allowedRoots, implicitExtension);
    }

    void ModuleIndex::RemoveFile(const std::string &filePath)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_includeGraph.RemoveFile(filePath);
    }

    std::vector<std::string> ModuleIndex::GetModuleClosure(const std::string &filePath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_includeGraph.GetModuleClosure(filePath);
    }

    void ModuleIndex::AssociateClosure(const std::string &openUri, std::vector<std::string> closureUris)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_openDocumentClosures[openUri] = std::move(closureUris);
    }

    std::vector<std::string> ModuleIndex::ReleaseClosure(const std::string &openUri)
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
        for (const auto &uriStr : released)
        {
            bool stillNeeded = false;
            for (const auto &[otherUri, otherClosure] : m_openDocumentClosures)
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

    void ModuleIndex::SetClosureDocument(const std::string &uri, std::string content)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closureDocuments[uri] = std::move(content);
    }

    bool ModuleIndex::HasClosureDocument(const std::string &uri) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closureDocuments.contains(uri);
    }

    std::optional<std::string> ModuleIndex::GetClosureDocument(const std::string &uri) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_closureDocuments.find(uri); it != m_closureDocuments.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    void ModuleIndex::PurgeClosureDocument(const std::string &uri)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closureDocuments.erase(uri);
    }

    void ModuleIndex::SetIndexedUri(const std::string &path, const std::string &uri)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_indexedUriByPath[path] = uri;
    }

    std::optional<std::string> ModuleIndex::GetIndexedUri(const std::string &path) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_indexedUriByPath.find(path); it != m_indexedUriByPath.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    void ModuleIndex::RemoveIndexedPath(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_indexedUriByPath.erase(path);
    }

    void ModuleIndex::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_includeGraph.Clear();
        m_closureDocuments.clear();
        m_openDocumentClosures.clear();
        m_indexedUriByPath.clear();
    }
}
