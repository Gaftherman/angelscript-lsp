#include "lsp/state/PredefinedStubManager.h"

#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{

void PredefinedStubManager::RegisterStub(const std::string& path, const std::string& uri, std::string content)
{
    const std::string canon = CanonicalizeStubPath(path);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!path.empty())
    {
        m_predefinedUriByPath[path] = uri;
    }
    if (!canon.empty())
    {
        m_loadedCanonicalPaths.insert(canon);
    }
    m_predefinedUris.insert(uri);
    m_predefinedDocuments[uri] = std::make_shared<std::string>(std::move(content));
}

bool PredefinedStubManager::HasStub(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_predefinedUris.contains(uri);
}

std::optional<std::string> PredefinedStubManager::GetDocumentText(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_predefinedDocuments.find(uri); it != m_predefinedDocuments.end() && it->second)
    {
        return *it->second;
    }
    return std::nullopt;
}

void PredefinedStubManager::SetDocumentText(const std::string& uri, std::string content)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_predefinedDocuments[uri] = std::make_shared<std::string>(std::move(content));
    m_predefinedUris.insert(uri);
}

void PredefinedStubManager::SetPreindexedDocument(const std::string& uri, std::shared_ptr<const document::Document> doc)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_preindexedSnapshots[uri] = std::move(doc);
    m_predefinedUris.insert(uri);
}

std::shared_ptr<const document::Document> PredefinedStubManager::GetPreindexedDocument(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_preindexedSnapshots.find(uri); it != m_preindexedSnapshots.end())
    {
        return it->second;
    }
    return nullptr;
}

std::optional<std::string> PredefinedStubManager::GetUriByPath(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_predefinedUriByPath.find(path); it != m_predefinedUriByPath.end())
    {
        return it->second;
    }
    const std::string canon = CanonicalizeStubPath(path);
    if (!canon.empty() && canon != path)
    {
        if (auto it = m_predefinedUriByPath.find(canon); it != m_predefinedUriByPath.end())
        {
            return it->second;
        }
    }
    return std::nullopt;
}

void PredefinedStubManager::SetUriForPath(const std::string& path, const std::string& uri)
{
    const std::string canon = CanonicalizeStubPath(path);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!path.empty())
    {
        m_predefinedUriByPath[path] = uri;
    }
    if (!canon.empty())
    {
        m_loadedCanonicalPaths.insert(canon);
    }
}

void PredefinedStubManager::RemoveStub(const std::string& uri)
{
    UnloadUri(uri, nullptr);
}

std::vector<std::pair<std::string, std::string>> PredefinedStubManager::GetAllUriByPath() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(m_predefinedUriByPath.size());
    for (const auto& entry : m_predefinedUriByPath)
    {
        result.emplace_back(entry.first, entry.second);
    }
    return result;
}

std::vector<std::string> PredefinedStubManager::GetLoadedUris() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> uris;
    uris.reserve(m_predefinedUris.size());
    for (const auto& uri : m_predefinedUris)
    {
        uris.push_back(uri);
    }
    return uris;
}

bool PredefinedStubManager::Contributes(const std::string& uri, const std::string& activeStub) const
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
    m_loadedCanonicalPaths.clear();
    m_fingerprints.clear();
    m_predefinedDocuments.clear();
    m_preindexedSnapshots.clear();
}

std::shared_ptr<const std::string> PredefinedStubManager::GetDocumentTextShared(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_predefinedDocuments.find(uri); it != m_predefinedDocuments.end())
    {
        return it->second;
    }
    return nullptr;
}

size_t PredefinedStubManager::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_predefinedUris.size();
}

} // namespace angel_lsp
