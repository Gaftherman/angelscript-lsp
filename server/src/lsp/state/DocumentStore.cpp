#include "lsp/state/DocumentStore.h"

#include "parser/SyntaxTree.h"

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
    m_documents.insert_or_assign(request.uri,
                                 std::make_shared<const document::Document>(
                                     document::DocumentSnapshot{request.uri, std::move(request.text), request.version,
                                                                gen, std::move(request.transformedSource)},
                                     std::move(request.tree)));
}

void DocumentStore::OpenDocument(std::string_view uri, std::string text, int version, parser::TreePtr tree)
{
    OpenDocument(OpenDocumentRequest{std::string(uri), std::move(text), version, std::move(tree), ""});
}

void DocumentStore::LinkDocument(std::string_view uri, std::shared_ptr<const document::Document> document,
                                 std::string_view clientUri, int version)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (!clientUri.empty())
    {
        m_clientUriByKey[std::string(uri)] = std::string(clientUri);
    }
    const uint64_t gen = m_nextGeneration++;
    if (document)
    {
        parser::TreePtr treeCopy = parser::CopyTree(document->tree.get());
        auto docWithVersion = std::make_shared<document::Document>(
            document::DocumentSnapshot{std::string(uri), document->text, version, gen, document->TransformedSource()},
            std::move(treeCopy));
        m_documents.insert_or_assign(std::string(uri), std::move(docWithVersion));
    }
}

void DocumentStore::UpdateDocument(std::string_view uri, std::string text, int version, parser::TreePtr tree,
                                   std::shared_ptr<const std::string> transformedSource)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end())
    {
        const uint64_t gen = it->second ? it->second->generation : m_nextGeneration++;
        it->second = std::make_shared<const document::Document>(
            document::DocumentSnapshot{std::string(uri), std::move(text), version, gen, std::move(transformedSource)},
            std::move(tree));
    }
    else
    {
        const uint64_t gen = m_nextGeneration++;
        m_documents.insert_or_assign(
            std::string(uri),
            std::make_shared<const document::Document>(
                document::DocumentSnapshot{std::string(uri), std::move(text), version, gen, std::move(transformedSource)},
                std::move(tree)));
    }
}

void DocumentStore::CloseDocument(std::string_view uri)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_documents.erase(uri);
}

bool DocumentStore::IsOpen(std::string_view uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_documents.contains(uri);
}

std::optional<std::string> DocumentStore::GetText(std::string_view uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        return it->second->text;
    }
    return std::nullopt;
}

int DocumentStore::GetVersion(std::string_view uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        return it->second->version;
    }
    return -1;
}

void DocumentStore::SetVersion(std::string_view uri, int version)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        parser::TreePtr treeCopy = parser::CopyTree(it->second->tree.get());
        it->second = std::make_shared<const document::Document>(
            document::DocumentSnapshot{std::string(uri), it->second->text, version, it->second->generation,
                                       it->second->TransformedSource()},
            std::move(treeCopy));
    }
}

parser::TreePtr DocumentStore::GetTree(std::string_view uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        if (it->second->tree)
        {
            return parser::CopyTree(it->second->tree.get());
        }
    }
    return parser::MakeTreePtr(nullptr);
}

void DocumentStore::SetTree(std::string_view uri, parser::TreePtr tree)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        it->second = std::make_shared<const document::Document>(
            document::DocumentSnapshot{std::string(uri), it->second->text, it->second->version, it->second->generation},
            std::move(tree));
    }
}

std::string DocumentStore::GetClientUri(std::string_view uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_clientUriByKey.find(uri); it != m_clientUriByKey.end())
    {
        return it->second;
    }
    return std::string(uri);
}

void DocumentStore::SetClientUri(std::string_view uri, std::string_view clientUri)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_clientUriByKey[std::string(uri)] = std::string(clientUri);
}

void DocumentStore::RemoveClientUri(std::string_view uri)
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

uint64_t DocumentStore::GetGeneration(std::string_view uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        return it->second->generation;
    }
    return 0;
}

bool DocumentStore::IsCurrent(std::string_view uri, uint64_t generation, int version) const
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

std::shared_ptr<const document::Document> DocumentStore::GetDocument(std::string_view uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end())
    {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<const std::string> DocumentStore::GetTextShared(std::string_view uri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (auto it = m_documents.find(uri); it != m_documents.end() && it->second)
    {
        return std::shared_ptr<const std::string>(it->second, &it->second->text);
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
