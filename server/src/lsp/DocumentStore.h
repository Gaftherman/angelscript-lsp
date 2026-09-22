#pragma once

#include "document/Document.h"
#include <ankerl/unordered_dense.h>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

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

    DocumentStore(const DocumentStore&) = delete;
    DocumentStore& operator=(const DocumentStore&) = delete;
    DocumentStore(DocumentStore&&) = delete;
    DocumentStore& operator=(DocumentStore&&) = delete;

    /**
     * @brief Request parameters for opening a document in DocumentStore.
     */
    struct OpenDocumentRequest
    {
        std::string uri;
        std::string text;
        int version = 0;
        document::TreePtr tree = document::MakeTreePtr(nullptr);
        std::string clientUri;
    };

    /**
     * @brief Opens a document in the store using a bundled request struct.
     * @param request Bundled document parameters.
     */
    void OpenDocument(OpenDocumentRequest request);

    /**
     * @brief Opens a document in the store without clientUri.
     * @param uri Canonical URI key.
     * @param text Document source content.
     * @param version Document version.
     * @param tree Parsed syntax tree (transfers ownership).
     */
    void OpenDocument(const std::string& uri, std::string text, int version, document::TreePtr tree);

    /**
     * @brief Updates text, version, and syntax tree of an existing open document.
     * @param uri Canonical URI key.
     * @param text New document source content.
     * @param version New document version.
     * @param tree Newly parsed syntax tree.
     */
    void UpdateDocument(const std::string& uri, std::string text, int version, document::TreePtr tree);

    /**
     * @brief Removes a document when closed.
     * @param uri Canonical URI key.
     */
    void CloseDocument(const std::string& uri);

    /**
     * @brief Checks if document is currently open. Thread-safe.
     * @param uri Canonical URI key.
     * @return True if document is tracked as open.
     */
    [[nodiscard]] bool IsOpen(const std::string& uri) const;

    /**
     * @brief Gets a copy of document source text, or nullopt if not open. Thread-safe.
     * @param uri Canonical URI key.
     * @return Optional string containing document text.
     */
    [[nodiscard]] std::optional<std::string> GetText(const std::string& uri) const;

    /**
     * @brief Gets document version, or -1 if not tracked. Thread-safe.
     * @param uri Canonical URI key.
     * @return Document version number or -1.
     */
    [[nodiscard]] int GetVersion(const std::string& uri) const;

    /**
     * @brief Sets document version. Thread-safe.
     * @param uri Canonical URI key.
     * @param version Document version number.
     */
    void SetVersion(const std::string& uri, int version);

    /**
     * @brief Borrows raw pointer to syntax tree. Caller must NOT delete. Thread-safe.
     * @param uri Canonical URI key.
     * @return Raw TSTree pointer or nullptr.
     */
    [[nodiscard]] TSTree* GetTree(const std::string& uri) const;

    /**
     * @brief Sets or replaces the syntax tree for a document. Thread-safe.
     * @param uri Canonical URI key.
     * @param tree Managed TreePtr.
     */
    void SetTree(const std::string& uri, document::TreePtr tree);

    /**
     * @brief Gets the client URI spelling for a canonical URI key. Thread-safe.
     * @param uri Canonical URI key.
     * @return Client URI string, or uri if not recorded.
     */
    [[nodiscard]] std::string GetClientUri(const std::string& uri) const;

    /**
     * @brief Sets the client URI spelling. Thread-safe.
     * @param uri Canonical URI key.
     * @param clientUri Client URI string.
     */
    void SetClientUri(const std::string& uri, const std::string& clientUri);

    /**
     * @brief Removes the client URI spelling. Thread-safe.
     * @param uri Canonical URI key.
     */
    void RemoveClientUri(const std::string& uri);

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
     * @brief Gets current document open generation, or 0 if not open. Thread-safe.
     * @param uri Canonical URI key.
     * @return Monotonic generation counter.
     */
    [[nodiscard]] uint64_t GetGeneration(const std::string& uri) const;

    /**
     * @brief Verifies whether a work item matches the active open document state.
     * @param uri Canonical URI key.
     * @param generation Document generation at time of scheduling.
     * @param version Document version at time of scheduling.
     * @return True if document is open with identical generation and version >= current version.
     */
    [[nodiscard]] bool IsCurrent(const std::string& uri, uint64_t generation, int version) const;

    /**
     * @brief Returns a shared pointer snapshot of the document, or nullptr if not open. Thread-safe.
     * @param uri Canonical URI key.
     * @return Shared pointer to Document or nullptr.
     */
    [[nodiscard]] std::shared_ptr<const document::Document> GetDocument(const std::string& uri) const;

    /**
     * @brief Returns a shared pointer handle to the document text, preserving lifetime. Thread-safe.
     * @param uri Canonical URI key.
     * @return Shared pointer to const document text or nullptr.
     */
    [[nodiscard]] std::shared_ptr<const std::string> GetTextShared(const std::string& uri) const;

    /**
     * @brief Borrows pointer to document source text, or nullptr if not open. Thread-safe.
     * @param uri Canonical URI key.
     * @return Pointer to document text or nullptr.
     * @note Deprecated in favor of GetTextShared() to prevent dangling pointer risk.
     */
    [[deprecated("Use GetTextShared or GetDocument to prevent dangling references")]] [[nodiscard]] const std::string*
    GetTextPtr(const std::string& uri) const;

    /**
     * @brief Returns thread-safe snapshot list of all open documents as immutable handles.
     * @return Vector of shared_ptr to const Document.
     */
    [[nodiscard]] std::vector<std::shared_ptr<const document::Document>> GetAllDocuments() const;

    /**
     * @brief Returns number of open documents.
     * @return Count of open documents.
     */
    [[nodiscard]] size_t Size() const;

    /**
     * @brief Concurrency helper to get a document handle under shared lock.
     * @param[in] uri Document URI.
     * @return Shared pointer to const Document.
     */
    [[nodiscard]] std::shared_ptr<const document::Document> getDocument(const std::string& uri) const
    {
        return GetDocument(uri);
    }

    /**
     * @brief Concurrency helper to put a document into store under unique lock.
     * @param[in] uri Document URI.
     * @param[in] text Source text.
     * @param[in] version Document version.
     * @param[in] tree Parsed AST handle.
     */
    void putDocument(const std::string& uri, std::string text, int version, document::TreePtr tree)
    {
        OpenDocument(uri, std::move(text), version, std::move(tree));
    }

    /**
     * @brief Concurrency helper to remove a document from store under unique lock.
     * @param[in] uri Document URI.
     */
    void removeDocument(const std::string& uri)
    {
        CloseDocument(uri);
    }

  private:
    mutable std::shared_mutex m_mutex;
    uint64_t m_nextGeneration = 1;
    ankerl::unordered_dense::map<std::string, std::shared_ptr<const document::Document>> m_documents;
    ankerl::unordered_dense::map<std::string, std::string> m_clientUriByKey;
};
} // namespace angel_lsp
