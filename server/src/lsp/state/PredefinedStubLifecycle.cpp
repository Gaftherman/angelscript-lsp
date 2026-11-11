#include "lsp/state/PredefinedStubManager.h"

#include "utils/IncludeResolver.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>

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

bool PredefinedStubManager::HasMatchingFingerprint(const std::string& path, uint64_t contentHash,
                                                   uint64_t definesHash) const
{
    const std::string canon = CanonicalizeStubPath(path);
    if (canon.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_fingerprints.find(canon);
    if (it == m_fingerprints.end())
    {
        return false;
    }
    return it->second.contentHash == contentHash && it->second.definesHash == definesHash;
}

void PredefinedStubManager::RecordFingerprint(const std::string& path, uint64_t contentHash, uint64_t definesHash)
{
    const std::string canon = CanonicalizeStubPath(path);
    if (canon.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fingerprints[canon] = StubFingerprint{contentHash, definesHash};
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
        m_preindexedSnapshots.erase(previous);
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
    m_preindexedSnapshots.erase(uri);

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

} // namespace angel_lsp
