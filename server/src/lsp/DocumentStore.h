#pragma once

#include "document/Document.h"
#include <ankerl/unordered_dense.h>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <memory>

namespace angel_lsp
{
    /**
     * @brief Thread-safe storage and cache for open LSP documents, syntax trees, versions, and client URI spellings.
     */
    class DocumentStore
    {
    public:
        /**
         * @brief Constructs an empty DocumentStore.
         */
        DocumentStore() = default;

        /**
         * @brief Destructor clears all tracked documents and trees.
         */
        ~DocumentStore() = default;

        DocumentStore(const DocumentStore &) = delete;
        DocumentStore &operator=(const DocumentStore &) = delete;
        DocumentStore(DocumentStore &&) = delete;
        DocumentStore &operator=(DocumentStore &&) = delete;

        /**
         * @brief Opens a document in the store.
         * @param uri Canonical URI key.
         * @param text Document source content.
         * @param version Document version.
         * @param tree Parsed syntax tree (transfers ownership).
         * @param clientUri Original URI spelling sent by client.
         */
        void OpenDocument(const std::string &uri, std::string text, int version,
                          document::TreePtr tree, const std::string &clientUri = "");

        /**
         * @brief Updates text, version, and syntax tree of an existing open document.
         * @param uri Canonical URI key.
         * @param text New document source content.
         * @param version New document version.
         * @param tree Newly parsed syntax tree.
         */
        void UpdateDocument(const std::string &uri, std::string text, int version,
                            document::TreePtr tree);

        /**
         * @brief Removes a document when closed.
         * @param uri Canonical URI key.
         */
        void CloseDocument(const std::string &uri);

        /**
         * @brief Checks if document is currently open. Thread-safe.
         * @param uri Canonical URI key.
         * @return True if document is tracked as open.
         */
        [[nodiscard]] bool IsOpen(const std::string &uri) const;

        /**
         * @brief Gets a copy of document source text, or nullopt if not open. Thread-safe.
         * @param uri Canonical URI key.
         * @return Optional string containing document text.
         */
        [[nodiscard]] std::optional<std::string> GetText(const std::string &uri) const;

        /**
         * @brief Gets document version, or -1 if not tracked. Thread-safe.
         * @param uri Canonical URI key.
         * @return Document version number or -1.
         */
        [[nodiscard]] int GetVersion(const std::string &uri) const;

        /**
         * @brief Sets document version. Thread-safe.
         * @param uri Canonical URI key.
         * @param version Document version number.
         */
        void SetVersion(const std::string &uri, int version);

        /**
         * @brief Borrows raw pointer to syntax tree. Caller must NOT delete. Thread-safe.
         * @param uri Canonical URI key.
         * @return Raw TSTree pointer or nullptr.
         */
        [[nodiscard]] TSTree *GetTree(const std::string &uri) const;

        /**
         * @brief Sets or replaces the syntax tree for a document. Thread-safe.
         * @param uri Canonical URI key.
         * @param tree Managed TreePtr.
         */
        void SetTree(const std::string &uri, document::TreePtr tree);

        /**
         * @brief Gets the client URI spelling for a canonical URI key. Thread-safe.
         * @param uri Canonical URI key.
         * @return Client URI string, or uri if not recorded.
         */
        [[nodiscard]] std::string GetClientUri(const std::string &uri) const;

        /**
         * @brief Sets the client URI spelling. Thread-safe.
         * @param uri Canonical URI key.
         * @param clientUri Client URI string.
         */
        void SetClientUri(const std::string &uri, const std::string &clientUri);

        /**
         * @brief Returns a thread-safe snapshot of all open document URIs and texts.
         * @return Vector of pairs containing (uri, text).
         */
        [[nodiscard]] std::vector<std::pair<std::string, std::string>> GetSnapshot() const;

        /**
         * @brief Returns a thread-safe list of all open document URIs.
         * @return Vector of canonical URI strings.
         */
        [[nodiscard]] std::vector<std::string> GetOpenUris() const;

        /**
         * @brief Clears all stored documents and syntax trees.
         */
        void Clear();

        /**
         * @brief Returns number of open documents.
         * @return Count of open documents.
         */
        [[nodiscard]] size_t Size() const;

    private:
        mutable std::mutex m_mutex;
        ankerl::unordered_dense::map<std::string, document::Document> m_documents;
        ankerl::unordered_dense::map<std::string, std::string> m_clientUriByKey;
    };
}
