#include "lsp/state/SyntaxRevisionCache.h"

#include <algorithm>

namespace angel_lsp
{
std::shared_ptr<const ParsedDocumentSyntax> SyntaxRevisionCache::Find(const SyntaxRevisionKey& key)
{
    if (key.generation == 0 && key.closedRevision == 0)
    {
        return {};
    }
    const auto found = std::ranges::find_if(m_entries,
                                            [&](const Entry& entry)
                                            {
                                                return entry.generation == key.generation &&
                                                       entry.closedRevision == key.closedRevision &&
                                                       entry.uri == key.uri && entry.source == key.source;
                                            });
    if (found == m_entries.end())
    {
        return {};
    }
    m_entries.splice(m_entries.begin(), m_entries, found);
    return m_entries.front().syntax;
}

void SyntaxRevisionCache::Remember(const SyntaxRevisionKey& key,
                                   const std::shared_ptr<const ParsedDocumentSyntax>& syntax)
{
    if ((key.generation == 0 && key.closedRevision == 0) || !syntax->tree || syntax->index.NodeCount() > MaxNodes ||
        key.source.size() > MaxSourceBytes)
    {
        return;
    }
    Forget(key.uri);
    m_entries.push_front({std::string(key.uri), std::string(key.source), key.generation, key.closedRevision, syntax});

    size_t nodes = 0;
    size_t bytes = 0;
    size_t entries = 0;
    auto end = m_entries.begin();
    for (; end != m_entries.end(); ++end)
    {
        nodes += end->syntax->index.NodeCount();
        bytes += end->source.size();
        if (++entries > MaxEntries || nodes > MaxNodes || bytes > MaxSourceBytes)
        {
            break;
        }
    }
    m_entries.erase(end, m_entries.end());
}

void SyntaxRevisionCache::Forget(std::string_view uri)
{
    m_entries.remove_if([&](const Entry& entry) { return entry.uri == uri; });
}
} // namespace angel_lsp
