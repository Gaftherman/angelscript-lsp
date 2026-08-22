#include "analysis/ScopeTree.h"

namespace angel_lsp::analysis
{
    const LocalDefinition *ResolveInScope(const Scope *scope, std::string_view name)
    {
        for (const Scope *current = scope; current != nullptr; current = current->parent)
        {
            for (const LocalDefinition &def : current->definitions)
            {
                if (def.name == name)
                    return &def;
            }
        }

        return nullptr;
    }

    void ScopeIndex::SetScopeTree(const std::string &fileUri, std::unique_ptr<Scope> root)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_roots[fileUri] = std::shared_ptr<Scope>(std::move(root));
    }

    void ScopeIndex::ClearDocument(const std::string &fileUri)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_roots.erase(fileUri);
    }

    std::shared_ptr<const Scope> ScopeIndex::GetRoot(const std::string &fileUri) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);

        auto it = m_roots.find(fileUri);
        if (it == m_roots.end())
            return nullptr;

        return it->second;
    }
}
