#include "lsp/WorkspaceStateStore.h"
#include "utils/Utils.h"

#include <filesystem>

namespace angel_lsp
{

WorkspaceStateStore::WorkspaceStateStore()
    : m_searchDirectories(std::make_shared<const std::vector<std::string>>()),
      m_definedWords(std::make_shared<const ankerl::unordered_dense::set<std::string>>())
{
}

WorkspaceSnapshot WorkspaceStateStore::CreateSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return WorkspaceSnapshot{m_searchDirectories, m_workspaceRoots, m_definedWords};
}

std::vector<std::string> WorkspaceStateStore::GetWorkspaceRoots() const
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_workspaceRoots;
}

void WorkspaceStateStore::SetWorkspaceRoots(std::vector<std::string> roots)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_workspaceRoots = std::move(roots);
}

std::shared_ptr<const std::vector<std::string>> WorkspaceStateStore::GetSearchDirectories() const
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_searchDirectories;
}

void WorkspaceStateStore::SetSearchDirectories(std::vector<std::string> searchDirs)
{
    SetSearchDirectories(std::make_shared<const std::vector<std::string>>(std::move(searchDirs)));
}

void WorkspaceStateStore::SetSearchDirectories(std::shared_ptr<const std::vector<std::string>> searchDirs)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_searchDirectories = std::move(searchDirs);
}

std::string WorkspaceStateStore::GetEngineProfile() const
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_engineProfile;
}

void WorkspaceStateStore::SetEngineProfile(std::string profile)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_engineProfile = std::move(profile);
}

std::shared_ptr<const ankerl::unordered_dense::set<std::string>> WorkspaceStateStore::GetDefinedWords() const
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_definedWords;
}

bool WorkspaceStateStore::SetDefinedWordsFrom(const std::string& source, std::vector<std::string> words)
{
    auto merged = std::make_shared<ankerl::unordered_dense::set<std::string>>();

    std::lock_guard<std::mutex> lock(m_configMutex);
    if (words.empty())
    {
        m_definedWordsBySource.erase(source);
    }
    else
    {
        m_definedWordsBySource[source] = std::move(words);
    }

    for (const auto& [_, contributed] : m_definedWordsBySource)
    {
        for (const auto& word : contributed)
        {
            merged->insert(word);
        }
    }

    if (m_definedWords && *m_definedWords == *merged)
    {
        return false;
    }

    m_definedWords = std::move(merged);
    return true;
}

std::vector<std::string> WorkspaceStateStore::GetAffectedDocuments(const std::string& changedUri) const
{
    const std::string changedPath = utils::UriToPath(changedUri);
    if (changedPath.empty())
    {
        return {};
    }

    const std::vector<std::string> dependentPaths = m_includeGraph.GetFilesIncluding(changedPath);
    std::vector<std::string> result;
    result.reserve(dependentPaths.size());

    for (const auto& depPath : dependentPaths)
    {
        std::string depUri = utils::PathToUri(depPath);
        if (!depUri.empty())
        {
            result.push_back(std::move(depUri));
        }
    }
    return result;
}

std::vector<std::string>
WorkspaceStateStore::PermittedIncludeDirectories(const std::vector<std::string>& configuredStubPaths) const
{
    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        for (const auto& wsRoot : m_workspaceRoots)
        {
            std::string path = utils::UriToPath(wsRoot);
            if (!path.empty())
            {
                roots.push_back(std::move(path));
            }
        }

        if (m_searchDirectories)
        {
            roots.insert(roots.end(), m_searchDirectories->begin(), m_searchDirectories->end());
        }
    }

    for (const auto& predefined : configuredStubPaths)
    {
        std::filesystem::path configured(predefined);
        if (configured.has_parent_path())
        {
            roots.push_back(configured.parent_path().string());
        }
    }
    return roots;
}

void WorkspaceStateStore::Clear()
{
    m_documentStore.Clear();
    m_includeGraph.Clear();
    m_moduleIndex.Clear();

    std::lock_guard<std::mutex> lock(m_configMutex);
    m_workspaceRoots.clear();
    m_searchDirectories = std::make_shared<const std::vector<std::string>>();
    m_engineProfile.clear();
    m_definedWords = std::make_shared<const ankerl::unordered_dense::set<std::string>>();
    m_definedWordsBySource.clear();
}

} // namespace angel_lsp
