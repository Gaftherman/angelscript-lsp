#pragma once

#include "document/Document.h"
#include "parser/SyntaxTree.h"
#include "utils/TransparentHash.h"

#include <ankerl/unordered_dense.h>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
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
        parser::TreePtr tree = parser::MakeTreePtr(nullptr);
        std::string clientUri;
        std::shared_ptr<const std::string> transformedSource;
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
    void OpenDocument(std::string_view uri, std::string text, int version, parser::TreePtr tree);

    /**
     * @brief Links an existing immutable Document snapshot into the open document store.
     * @param uri Canonical URI key.
     * @param document Shared pointer to immutable Document snapshot.
     * @param clientUri Optional client URI spelling.
     */
    void LinkDocument(std::string_view uri, std::shared_ptr<const document::Document> document,
                      std::string_view clientUri = "", int version = 0);

    /**
     * @brief Updates text, version, and syntax tree of an existing open document.
     * @param uri Canonical URI key.
     * @param text New document source content.
     * @param version New document version.
     * @param tree Newly parsed syntax tree.
     */
    void UpdateDocument(std::string_view uri, std::string text, int version, parser::TreePtr tree,
                        std::shared_ptr<const std::string> transformedSource = {});

    /**
     * @brief Removes a document when closed.
     * @param uri Canonical URI key.
     */
    void CloseDocument(std::string_view uri);

    /**
     * @brief Checks if document is currently open. Thread-safe.
     * @param uri Canonical URI key.
     * @return True if document is tracked as open.
     */
    [[nodiscard]] bool IsOpen(std::string_view uri) const;

    /**
     * @brief Gets a copy of document source text, or nullopt if not open. Thread-safe.
     * @param uri Canonical URI key.
     * @return Optional string containing document text.
     */
    [[nodiscard]] std::optional<std::string> GetText(std::string_view uri) const;

    /**
     * @brief Gets document version, or -1 if not tracked. Thread-safe.
     * @param uri Canonical URI key.
     * @return Document version number or -1.
     */
    [[nodiscard]] int GetVersion(std::string_view uri) const;

    /**
     * @brief Sets document version. Thread-safe.
     * @param uri Canonical URI key.
     * @param version Document version number.
     */
    void SetVersion(std::string_view uri, int version);

    /**
     * @brief Returns a thread-local copy of the document's syntax tree via ts_tree_copy(). Thread-safe.
     * @param uri Canonical URI key.
     * @return Managed TreePtr owning the copied tree, or nullptr.
     */
    [[nodiscard]] parser::TreePtr GetTree(std::string_view uri) const;

    /**
     * @brief Sets or replaces the syntax tree for a document. Thread-safe.
     * @param uri Canonical URI key.
     * @param tree Managed TreePtr.
     */
    void SetTree(std::string_view uri, parser::TreePtr tree);

    /**
     * @brief Gets the client URI spelling for a canonical URI key. Thread-safe.
     * @param uri Canonical URI key.
     * @return Client URI string, or uri if not recorded.
     */
    [[nodiscard]] std::string GetClientUri(std::string_view uri) const;

    /**
     * @brief Sets the client URI spelling. Thread-safe.
     * @param uri Canonical URI key.
     * @param clientUri Client URI string.
     */
    void SetClientUri(std::string_view uri, std::string_view clientUri);

    /**
     * @brief Removes the client URI spelling. Thread-safe.
     * @param uri Canonical URI key.
     */
    void RemoveClientUri(std::string_view uri);

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
    [[nodiscard]] uint64_t GetGeneration(std::string_view uri) const;

    /**
     * @brief Verifies whether a work item matches the active open document state.
     * @param uri Canonical URI key.
     * @param generation Document generation at time of scheduling.
     * @param version Document version at time of scheduling.
     * @return True if document is open with identical generation and version >= current version.
     */
    [[nodiscard]] bool IsCurrent(std::string_view uri, uint64_t generation, int version) const;

    /**
     * @brief Returns a shared pointer snapshot of the document, or nullptr if not open. Thread-safe.
     * @param uri Canonical URI key.
     * @return Shared pointer to Document or nullptr.
     */
    [[nodiscard]] std::shared_ptr<const document::Document> GetDocument(std::string_view uri) const;

    /**
     * @brief Returns a shared pointer handle to the document text, preserving lifetime. Thread-safe.
     * @param uri Canonical URI key.
     * @return Shared pointer to const document text or nullptr.
     */
    [[nodiscard]] std::shared_ptr<const std::string> GetTextShared(std::string_view uri) const;

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
    [[nodiscard]] std::shared_ptr<const document::Document> getDocument(std::string_view uri) const
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
    void putDocument(std::string_view uri, std::string text, int version, parser::TreePtr tree)
    {
        OpenDocument(uri, std::move(text), version, std::move(tree));
    }

    /**
     * @brief Concurrency helper to remove a document from store under unique lock.
     * @param[in] uri Document URI.
     */
    void removeDocument(std::string_view uri)
    {
        CloseDocument(uri);
    }

  private:
    mutable std::shared_mutex m_mutex;
    uint64_t m_nextGeneration = 1;
    ankerl::unordered_dense::map<std::string, std::shared_ptr<const document::Document>, TransparentStringHash,
                                 std::equal_to<>>
        m_documents;
    ankerl::unordered_dense::map<std::string, std::string, TransparentStringHash, std::equal_to<>> m_clientUriByKey;
};
} // namespace angel_lsp
