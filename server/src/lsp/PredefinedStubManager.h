#pragma once

#include <ankerl/unordered_dense.h>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace angel_lsp
{
    /**
     * @brief Thread-safe manager for loading, caching, and discovery of predefined API stubs.
     */
    class PredefinedStubManager
    {
    public:
        /**
         * @brief Constructs an empty PredefinedStubManager.
         */
        PredefinedStubManager() = default;

        /**
         * @brief Destructor clears all tracked stub entries.
         */
        ~PredefinedStubManager() = default;

        PredefinedStubManager(const PredefinedStubManager &) = delete;
        PredefinedStubManager &operator=(const PredefinedStubManager &) = delete;
        PredefinedStubManager(PredefinedStubManager &&) = delete;
        PredefinedStubManager &operator=(PredefinedStubManager &&) = delete;

        /**
         * @brief Registers or updates a predefined stub document and its canonical path.
         * @param path Canonical filesystem path.
         * @param uri URI under which stub is indexed.
         * @param content Stub source text.
         */
        void RegisterStub(const std::string &path, const std::string &uri, std::string content);

        /**
         * @brief Checks if a stub URI is registered.
         * @param uri Stub URI.
         * @return True if registered.
         */
        [[nodiscard]] bool HasStub(const std::string &uri) const;

        /**
         * @brief Gets source text for a predefined stub URI, if loaded.
         * @param uri Stub URI.
         * @return Optional string containing stub source content.
         */
        [[nodiscard]] std::optional<std::string> GetDocumentText(const std::string &uri) const;

        /**
         * @brief Updates source content of an existing predefined stub.
         * @param uri Stub URI.
         * @param content New stub source text.
         */
        void SetDocumentText(const std::string &uri, std::string content);

        /**
         * @brief Looks up indexed URI for a given filesystem path.
         * @param path Canonical filesystem path.
         * @return Optional URI string.
         */
        [[nodiscard]] std::optional<std::string> GetUriByPath(const std::string &path) const;

        /**
         * @brief Associates a filesystem path with an indexed URI.
         * @param path Canonical filesystem path.
         * @param uri Indexed URI.
         */
        void SetUriForPath(const std::string &path, const std::string &uri);

        /**
         * @brief Removes a predefined stub by URI.
         * @param uri Stub URI to remove.
         */
        void RemoveStub(const std::string &uri);

        /**
         * @brief Returns list of all registered stub URIs.
         * @return Vector of URI strings.
         */
        [[nodiscard]] std::vector<std::string> GetLoadedUris() const;

        /**
         * @brief Determines whether a stub contributes symbols under active stub selection filter.
         * @param uri Stub URI.
         * @param activeStub Optional active stub path filter (empty means all contribute).
         * @return True if the stub should be loaded and contribute symbols.
         */
        [[nodiscard]] bool Contributes(const std::string &uri, const std::string &activeStub = "") const;

        /**
         * @brief Clears all registered stubs and path mappings.
         */
        void Clear();

        /**
         * @brief Returns total number of registered stubs.
         */
        [[nodiscard]] size_t Size() const;

    private:
        mutable std::mutex m_mutex;
        ankerl::unordered_dense::set<std::string> m_predefinedUris;
        ankerl::unordered_dense::map<std::string, std::string> m_predefinedUriByPath;
        ankerl::unordered_dense::map<std::string, std::string> m_predefinedDocuments;
    };
}
