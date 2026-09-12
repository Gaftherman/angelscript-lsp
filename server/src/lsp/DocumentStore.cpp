#include "lsp/DocumentStore.h"

namespace angel_lsp
{
    void DocumentStore::OpenDocument(const std::string &uri, std::string text, int version,
                                     document::TreePtr tree, const std::string &clientUri)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!clientUri.empty())
        {
            m_clientUriByKey[uri] = clientUri;
        }

        m_documents.insert_or_assign(uri, document::Document{ uri, std::move(text), version, std::move(tree) });
    }

    void DocumentStore::UpdateDocument(const std::string &uri, std::string text, int version,
                                       document::TreePtr tree)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_documents.find(uri); it != m_documents.end())
        {
            it->second.text = std::move(text);
            it->second.version = version;
            it->second.tree = std::move(tree);
        }
        else
        {
            m_documents.insert_or_assign(uri, document::Document{ uri, std::move(text), version, std::move(tree) });
        }
    }

    void DocumentStore::CloseDocument(const std::string &uri)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_documents.erase(uri);
    }

    bool DocumentStore::IsOpen(const std::string &uri) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_documents.contains(uri);
    }

    std::optional<std::string> DocumentStore::GetText(const std::string &uri) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_documents.find(uri); it != m_documents.end())
        {
            return it->second.text;
        }
        return std::nullopt;
    }

    int DocumentStore::GetVersion(const std::string &uri) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_documents.find(uri); it != m_documents.end())
        {
            return it->second.version;
        }
        return -1;
    }

    void DocumentStore::SetVersion(const std::string &uri, int version)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_documents.find(uri); it != m_documents.end())
        {
            it->second.version = version;
        }
    }

    TSTree *DocumentStore::GetTree(const std::string &uri) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_documents.find(uri); it != m_documents.end())
        {
            return it->second.tree.get();
        }
        return nullptr;
    }

    void DocumentStore::SetTree(const std::string &uri, document::TreePtr tree)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_documents.find(uri); it != m_documents.end())
        {
            it->second.tree = std::move(tree);
        }
    }

    std::string DocumentStore::GetClientUri(const std::string &uri) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_clientUriByKey.find(uri); it != m_clientUriByKey.end())
        {
            return it->second;
        }
        return uri;
    }

    void DocumentStore::SetClientUri(const std::string &uri, const std::string &clientUri)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_clientUriByKey[uri] = clientUri;
    }

    std::vector<std::pair<std::string, std::string>> DocumentStore::GetSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::pair<std::string, std::string>> snapshot;
        snapshot.reserve(m_documents.size());
        for (const auto &[uri, doc] : m_documents)
        {
            snapshot.emplace_back(uri, doc.text);
        }
        return snapshot;
    }

    std::vector<std::string> DocumentStore::GetOpenUris() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> uris;
        uris.reserve(m_documents.size());
        for (const auto &[uri, doc] : m_documents)
        {
            uris.push_back(uri);
        }
        return uris;
    }

    void DocumentStore::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_documents.clear();
        m_clientUriByKey.clear();
    }

    size_t DocumentStore::Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_documents.size();
    }
}
