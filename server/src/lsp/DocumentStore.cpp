#include "lsp/DocumentStore.h"
#include <mutex>

namespace angel_lsp
{
void DocumentStore::OpenDocument(OpenDocumentRequest request)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (!request.clientUri.empty())
    {
        m_clientUriByKey[request.uri] = request.clientUri;
    }

    const uint64_t gen = m_nextGeneration++;
    m_documents.insert_or_assign(
        request.uri, std::make_shared<const document::Document>(
                         document::DocumentSnapshot{request.uri, std::move(request.text), request.version, gen},
                         std::move(request.tree)));
}

void DocumentStore::OpenDocument(const std::string& uri, std::string text, int version, document::TreePtr tree)
{
    OpenDocument(OpenDocumentRequest{uri, std::move(text), version, std::move(tree), ""});
}

void DocumentStore::UpdateDocument(const std::string& uri, std::string text, int version, document::TreePtr tree)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end())
    {
        const uint64_t gen = it->second ? it->second->generation : m_nextGeneration++;
        it->second = std::make_shared<const document::Document>(
            document::DocumentSnapshot{uri, std::move(text), version, gen}, std::move(tree));
    }
    else
    {
        const uint64_t gen = m_nextGeneration++;
        m_documents.insert_or_assign(
            uri, std::make_shared<const document::Document>(
                     document::DocumentSnapshot{uri, std::move(text), version, gen}, std::move(tree)));
    }
}

void DocumentStore::CloseDocument(const std::string& uri)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_documents.erase(uri);
}

bool DocumentStore::IsOpen(const std::string& uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_documents.contains(uri);
}

std::optional<std::string> DocumentStore::GetText(const std::string& uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        return it->second->text;
    }
    return std::nullopt;
}

int DocumentStore::GetVersion(const std::string& uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        return it->second->version;
    }
    return -1;
}

void DocumentStore::SetVersion(const std::string& uri, int version)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        document::TreePtr treeCopy =
            document::MakeTreePtr(it->second->tree ? ts_tree_copy(it->second->tree.get()) : nullptr);
        it->second = std::make_shared<const document::Document>(
            document::DocumentSnapshot{uri, it->second->text, version, it->second->generation}, std::move(treeCopy));
    }
}

document::TreePtr DocumentStore::GetTree(const std::string& uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second && it->second->tree)
    {
        return document::MakeTreePtr(ts_tree_copy(it->second->tree.get()));
    }
    return document::MakeTreePtr(nullptr);
}

document::TreePtr DocumentStore::GetTreeCopy(const std::string& uri) const
{
    return GetTree(uri);
}

void DocumentStore::SetTree(const std::string& uri, document::TreePtr tree)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        it->second = std::make_shared<const document::Document>(
            document::DocumentSnapshot{uri, it->second->text, it->second->version, it->second->generation},
            std::move(tree));
    }
}

std::string DocumentStore::GetClientUri(const std::string& uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_clientUriByKey.find(uri); it != m_clientUriByKey.end())
    {
        return it->second;
    }
    return uri;
}

void DocumentStore::SetClientUri(const std::string& uri, const std::string& clientUri)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_clientUriByKey[uri] = clientUri;
}

void DocumentStore::RemoveClientUri(const std::string& uri)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_clientUriByKey.erase(uri);
}

std::vector<std::pair<std::string, std::string>> DocumentStore::GetSnapshot() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<std::pair<std::string, std::string>> snapshot;
    snapshot.reserve(m_documents.size());
    for (const auto& [uri, doc] : m_documents)
    {
        if (doc)
        {
            snapshot.emplace_back(uri, doc->text);
        }
    }
    return snapshot;
}

std::vector<std::string> DocumentStore::GetOpenUris() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<std::string> uris;
    uris.reserve(m_documents.size());
    for (const auto& [uri, doc] : m_documents)
    {
        uris.push_back(uri);
    }
    return uris;
}

void DocumentStore::Clear()
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_documents.clear();
    m_clientUriByKey.clear();
}

uint64_t DocumentStore::GetGeneration(const std::string& uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        return it->second->generation;
    }
    return 0;
}

bool DocumentStore::IsCurrent(const std::string& uri, uint64_t generation, int version) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_documents.find(uri);
    if (it == m_documents.end() || !it->second)
    {
        return false;
    }
    if (generation > 0 && it->second->generation != generation)
    {
        return false;
    }
    if (version >= 0 && it->second->version > version)
    {
        return false;
    }
    return true;
}

std::shared_ptr<const document::Document> DocumentStore::GetDocument(const std::string& uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end())
    {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<const std::string> DocumentStore::GetTextShared(const std::string& uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        return std::shared_ptr<const std::string>(it->second, &it->second->text);
    }
    return nullptr;
}

const std::string* DocumentStore::GetTextPtr(const std::string& uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        return &it->second->text;
    }
    return nullptr;
}

std::vector<std::shared_ptr<const document::Document>> DocumentStore::GetAllDocuments() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<std::shared_ptr<const document::Document>> docs;
    docs.reserve(m_documents.size());
    for (const auto& [uri, doc] : m_documents)
    {
        if (doc)
        {
            docs.push_back(doc);
        }
    }
    return docs;
}

size_t DocumentStore::Size() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_documents.size();
}
} // namespace angel_lsp
