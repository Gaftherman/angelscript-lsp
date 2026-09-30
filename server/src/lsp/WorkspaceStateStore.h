#pragma once

#include "document/Document.h"
#include "lsp/DocumentStore.h"
#include "lsp/ModuleIndex.h"
#include "utils/WorkspaceIncludeGraph.h"

#include <ankerl/unordered_dense.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp
{

/**
 * @brief Immutable snapshot of workspace-wide configuration and preprocessor state.
 */
struct WorkspaceSnapshot
{
    /** @brief Active search directories for include resolution. */
    std::shared_ptr<const std::vector<std::string>> searchDirectories;

    /** @brief Active workspace root directory URIs. */
    std::vector<std::string> workspaceRoots;

    /** @brief Active preprocessor defined symbols. */
    std::shared_ptr<const ankerl::unordered_dense::set<std::string>> definedWords;
};

/**
 * @brief Thread-safe manager for workspace document lifecycle, include topology,
 *        module indices, and workspace configuration snapshots.
 */
class WorkspaceStateStore
{
  public:
    /**
     * @brief Constructs an empty WorkspaceStateStore.
     */
    WorkspaceStateStore();

    /**
     * @brief Destructor.
     */
    ~WorkspaceStateStore() = default;

    WorkspaceStateStore(const WorkspaceStateStore&) = delete;
    WorkspaceStateStore& operator=(const WorkspaceStateStore&) = delete;
    WorkspaceStateStore(WorkspaceStateStore&&) = delete;
    WorkspaceStateStore& operator=(WorkspaceStateStore&&) = delete;

    /**
     * @brief Creates a thread-safe snapshot of the current workspace configuration.
     * @return Immutable WorkspaceSnapshot.
     */
    [[nodiscard]] WorkspaceSnapshot CreateSnapshot() const;

    /**
     * @brief Gets reference to the underlying document store.
     * @return Mutable reference to DocumentStore.
     */
    [[nodiscard]] DocumentStore& GetDocumentStore() noexcept
    {
        return m_documentStore;
    }

    /**
     * @brief Gets const reference to the underlying document store.
     * @return Const reference to DocumentStore.
     */
    [[nodiscard]] const DocumentStore& GetDocumentStore() const noexcept
    {
        return m_documentStore;
    }

    /**
     * @brief Gets reference to the underlying workspace include graph.
     * @return Mutable reference to WorkspaceIncludeGraph.
     */
    [[nodiscard]] utils::WorkspaceIncludeGraph& GetIncludeGraph() noexcept
    {
        return m_includeGraph;
    }

    /**
     * @brief Gets const reference to the underlying workspace include graph.
     * @return Const reference to WorkspaceIncludeGraph.
     */
    [[nodiscard]] const utils::WorkspaceIncludeGraph& GetIncludeGraph() const noexcept
    {
        return m_includeGraph;
    }

    /**
     * @brief Gets reference to the module index.
     * @return Mutable reference to ModuleIndex.
     */
    [[nodiscard]] ModuleIndex& GetModuleIndex() noexcept
    {
        return m_moduleIndex;
    }

    /**
     * @brief Gets const reference to the module index.
     * @return Const reference to ModuleIndex.
     */
    [[nodiscard]] const ModuleIndex& GetModuleIndex() const noexcept
    {
        return m_moduleIndex;
    }

    /**
     * @brief Retrieves a copy of the current workspace root URIs.
     * @return Vector of workspace root URIs.
     */
    [[nodiscard]] std::vector<std::string> GetWorkspaceRoots() const;

    /**
     * @brief Updates the workspace root URIs.
     * @param[in] roots New vector of workspace root URIs.
     */
    void SetWorkspaceRoots(std::vector<std::string> roots);

    /**
     * @brief Retrieves a thread-safe shared pointer to the current include search directories.
     * @return Shared pointer to const vector of search directory paths.
     */
    [[nodiscard]] std::shared_ptr<const std::vector<std::string>> GetSearchDirectories() const;

    /**
     * @brief Updates the include search directories.
     * @param[in] searchDirs New vector of search directory paths.
     */
    void SetSearchDirectories(std::vector<std::string> searchDirs);

    /**
     * @brief Updates the include search directories using an existing shared pointer.
     * @param[in] searchDirs Shared pointer to search directories.
     */
    void SetSearchDirectories(std::shared_ptr<const std::vector<std::string>> searchDirs);

    /**
     * @brief Retrieves the active engine profile name.
     * @return Current engine profile string.
     */
    [[nodiscard]] std::string GetEngineProfile() const;

    /**
     * @brief Updates the active engine profile name.
     * @param[in] profile New engine profile string.
     */
    void SetEngineProfile(std::string profile);

    /**
     * @brief Retrieves a thread-safe snapshot of defined preprocessor words.
     * @return Shared pointer to const set of defined words.
     */
    [[nodiscard]] std::shared_ptr<const ankerl::unordered_dense::set<std::string>> GetDefinedWords() const;

    /**
     * @brief Records preprocessor define contributions from a specific source.
     * @param[in] source Key identifying contributor (stub path or "" for config).
     * @param[in] words List of defined symbol words.
     * @return True if the aggregated set of defined words changed as a result.
     */
    bool SetDefinedWordsFrom(const std::string& source, std::vector<std::string> words);

    /**
     * @brief Computes list of documents affected by changes to the specified URI.
     * @param[in] changedUri Canonical URI of changed file.
     * @return Vector of URIs that depend on changedUri according to the include graph.
     */
    [[nodiscard]] std::vector<std::string> GetAffectedDocuments(const std::string& changedUri) const;

    /**
     * @brief Computes the list of directories an #include directive is permitted to resolve into.
     * @param[in] configuredStubPaths Explicit predefined stub file paths from configuration.
     * @return Vector of permitted directory paths.
     */
    [[nodiscard]] std::vector<std::string>
    PermittedIncludeDirectories(const std::vector<std::string>& configuredStubPaths) const;

    /**
     * @brief Caches a pre-indexed immutable Document snapshot for a URI.
     * @param[in] uri Canonical URI.
     * @param[in] doc Shared pointer to Document snapshot.
     */
    void SetPreindexedDocument(const std::string& uri, std::shared_ptr<const document::Document> doc);

    /**
     * @brief Retrieves pre-indexed immutable Document snapshot for a URI, if available.
     * @param[in] uri Canonical URI.
     * @return Shared pointer to Document snapshot or nullptr.
     */
    [[nodiscard]] std::shared_ptr<const document::Document> GetPreindexedDocument(const std::string& uri) const;

    /**
     * @brief Clears all document, include, module, and preprocessor state.
     */
    void Clear();

  private:
    DocumentStore m_documentStore;
    utils::WorkspaceIncludeGraph m_includeGraph;
    ModuleIndex m_moduleIndex;

    mutable std::mutex m_configMutex;
    std::vector<std::string> m_workspaceRoots;
    std::shared_ptr<const std::vector<std::string>> m_searchDirectories;
    std::string m_engineProfile;

    std::shared_ptr<const ankerl::unordered_dense::set<std::string>> m_definedWords;
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_definedWordsBySource;

    mutable std::mutex m_preindexedMutex;
    ankerl::unordered_dense::map<std::string, std::shared_ptr<const document::Document>> m_preindexedDocuments;
};

} // namespace angel_lsp
