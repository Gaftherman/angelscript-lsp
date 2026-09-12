#include "lsp/PredefinedStubManager.h"
#include "utils/IncludeResolver.h"

namespace angel_lsp
{
    void PredefinedStubManager::RegisterStub(const std::string &path, const std::string &uri, std::string content)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!path.empty())
        {
            m_predefinedUriByPath[path] = uri;
        }
        m_predefinedUris.insert(uri);
        m_predefinedDocuments[uri] = std::move(content);
    }

    bool PredefinedStubManager::HasStub(const std::string &uri) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_predefinedUris.contains(uri);
    }

    std::optional<std::string> PredefinedStubManager::GetDocumentText(const std::string &uri) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_predefinedDocuments.find(uri); it != m_predefinedDocuments.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    void PredefinedStubManager::SetDocumentText(const std::string &uri, std::string content)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_predefinedDocuments[uri] = std::move(content);
        m_predefinedUris.insert(uri);
    }

    std::optional<std::string> PredefinedStubManager::GetUriByPath(const std::string &path) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_predefinedUriByPath.find(path); it != m_predefinedUriByPath.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    void PredefinedStubManager::SetUriForPath(const std::string &path, const std::string &uri)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_predefinedUriByPath[path] = uri;
    }

    void PredefinedStubManager::RemoveStub(const std::string &uri)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_predefinedUris.erase(uri);
        m_predefinedDocuments.erase(uri);
        for (auto it = m_predefinedUriByPath.begin(); it != m_predefinedUriByPath.end(); ++it)
        {
            if (it->second == uri)
            {
                m_predefinedUriByPath.erase(it);
                break;
            }
        }
    }

    std::vector<std::string> PredefinedStubManager::GetLoadedUris() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> uris;
        uris.reserve(m_predefinedUris.size());
        for (const auto &uri : m_predefinedUris)
        {
            uris.push_back(uri);
        }
        return uris;
    }

    bool PredefinedStubManager::Contributes(const std::string &uri, const std::string &activeStub) const
    {
        if (activeStub.empty())
        {
            return true;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_predefinedUris.contains(uri))
        {
            return true;
        }

        return uri.find(activeStub) != std::string::npos;
    }

    void PredefinedStubManager::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_predefinedUris.clear();
        m_predefinedUriByPath.clear();
        m_predefinedDocuments.clear();
    }

    size_t PredefinedStubManager::Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_predefinedUris.size();
    }
}
