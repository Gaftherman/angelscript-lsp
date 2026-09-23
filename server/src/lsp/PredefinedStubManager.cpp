#include "lsp/PredefinedStubManager.h"
#include "utils/IncludeResolver.h"
#include <filesystem>

namespace angel_lsp
{
std::string PredefinedStubManager::CanonicalizeStubPath(const std::string& path)
{
    if (path.empty())
    {
        return "";
    }

#if !defined(_WIN32)
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
    {
        std::filesystem::path p(path);
        std::string s = p.lexically_normal().string();
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    }
#endif

    std::error_code ec;
    const std::filesystem::path p(path);
    if (std::filesystem::exists(p, ec))
    {
        const auto canon = std::filesystem::weakly_canonical(p, ec);
        if (!ec)
        {
            return angel_lsp::utils::IncludeResolver::NormalizePath(canon);
        }
    }

    const auto canon = std::filesystem::weakly_canonical(p, ec);
    return angel_lsp::utils::IncludeResolver::NormalizePath(ec ? p.lexically_normal() : canon);
}

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

bool PredefinedStubManager::IsCanonicalPathLoaded(const std::string& path) const
{
    const std::string canon = CanonicalizeStubPath(path);
    if (canon.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_loadedCanonicalPaths.contains(canon);
}

bool PredefinedStubManager::MarkCanonicalPathLoaded(const std::string& path)
{
    const std::string canon = CanonicalizeStubPath(path);
    if (canon.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_loadedCanonicalPaths.insert(canon).second;
}

bool PredefinedStubManager::ClaimFile(const std::string& uri, const std::string& path, bool forceReload,
                                      std::string* outPreviousUri)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (path.empty())
    {
        return m_predefinedUris.insert(uri).second || forceReload;
    }

    if (const auto owner = m_predefinedUriByPath.find(path); owner != m_predefinedUriByPath.end())
    {
        if (owner->second == uri)
        {
            return forceReload;
        }

        const std::string previous = owner->second;
        if (outPreviousUri)
        {
            *outPreviousUri = previous;
        }
        m_predefinedUris.erase(previous);
        m_predefinedDocuments.erase(previous);
    }

    m_predefinedUriByPath[path] = uri;
    const std::string canon = CanonicalizeStubPath(path);
    if (!canon.empty())
    {
        m_loadedCanonicalPaths.insert(canon);
    }
    m_predefinedUris.insert(uri);
    return true;
}

bool PredefinedStubManager::UnloadUri(const std::string& uri, std::string* outPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_predefinedUris.contains(uri))
    {
        return false;
    }

    m_predefinedUris.erase(uri);
    m_predefinedDocuments.erase(uri);

    for (auto it = m_predefinedUriByPath.begin(); it != m_predefinedUriByPath.end(); ++it)
    {
        if (it->second == uri)
        {
            if (outPath)
            {
                *outPath = it->first;
            }
            const std::string canon = CanonicalizeStubPath(it->first);
            if (!canon.empty())
            {
                m_loadedCanonicalPaths.erase(canon);
            }
            m_predefinedUriByPath.erase(it);
            break;
        }
    }
    return true;
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
    m_predefinedDocuments.clear();
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

const std::string* PredefinedStubManager::GetDocumentTextPtr(const std::string& uri) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_predefinedDocuments.find(uri); it != m_predefinedDocuments.end() && it->second)
    {
        return it->second.get();
    }
    return nullptr;
}

size_t PredefinedStubManager::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_predefinedUris.size();
}
} // namespace angel_lsp
