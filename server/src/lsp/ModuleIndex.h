#pragma once

#include "utils/WorkspaceIncludeGraph.h"
#include <ankerl/unordered_dense.h>
#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace angel_lsp
{
    /**
     * @brief Thread-safe manager for workspace include graphs, module closures, and cross-file indexing.
     */
    class ModuleIndex
    {
    public:
        /**
         * @brief Constructs an empty ModuleIndex.
         */
        ModuleIndex() = default;

        /**
         * @brief Destructor.
         */
        ~ModuleIndex() = default;

        ModuleIndex(const ModuleIndex &) = delete;
        ModuleIndex &operator=(const ModuleIndex &) = delete;
        ModuleIndex(ModuleIndex &&) = delete;
        ModuleIndex &operator=(ModuleIndex &&) = delete;

        /**
         * @brief Updates include dependencies for a saved or scanned file.
         */
        void UpdateFile(const std::string &filePath, std::string_view content,
                        const std::vector<std::string> &searchDirectories,
                        const std::vector<std::string> &allowedRoots = {},
                        std::string_view implicitExtension = {});

        /**
         * @brief Removes a file from the include graph.
         */
        void RemoveFile(const std::string &filePath);

        /**
         * @brief Computes transitive module closure of file paths for a root file.
         */
        [[nodiscard]] std::vector<std::string> GetModuleClosure(const std::string &filePath) const;

        /**
         * @brief Associates an open document with its required closure URIs.
         */
        void AssociateClosure(const std::string &openUri, std::vector<std::string> closureUris);

        /**
         * @brief Releases an open document's reference to its closure URIs.
         * @param openUri Canonical URI of open document closing.
         * @return Closure URIs that are no longer referenced by any open document.
         */
        std::vector<std::string> ReleaseClosure(const std::string &openUri);

        /**
         * @brief Stores text for a closure document.
         */
        void SetClosureDocument(const std::string &uri, std::string content);

        /**
         * @brief Checks if a closure document is currently cached.
         */
        [[nodiscard]] bool HasClosureDocument(const std::string &uri) const;

        /**
         * @brief Gets stored text for a closure document, if present.
         */
        [[nodiscard]] std::optional<std::string> GetClosureDocument(const std::string &uri) const;

        /**
         * @brief Purges a closure document from cache.
         */
        void PurgeClosureDocument(const std::string &uri);

        /**
         * @brief Maps a canonical filesystem path to its indexed URI.
         */
        void SetIndexedUri(const std::string &path, const std::string &uri);

        /**
         * @brief Looks up indexed URI for a canonical filesystem path.
         */
        [[nodiscard]] std::optional<std::string> GetIndexedUri(const std::string &path) const;

        /**
         * @brief Removes indexed path entry.
         */
        void RemoveIndexedPath(const std::string &path);

        /**
         * @brief Clears all include graph and closure state.
         */
        void Clear();

    private:
        mutable std::mutex m_mutex;
        utils::WorkspaceIncludeGraph m_includeGraph;
        ankerl::unordered_dense::map<std::string, std::string> m_closureDocuments;
        ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_openDocumentClosures;
        ankerl::unordered_dense::map<std::string, std::string> m_indexedUriByPath;
    };
}
