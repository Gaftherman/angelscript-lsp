#include "lsp/state/SyntaxRevisionCache.h"

#include "parser/SyntaxTree.h"
#include "utils/Timer.h"

#include <algorithm>

namespace angel_lsp
{
std::list<SyntaxRevisionCache::Entry>::iterator SyntaxRevisionCache::RetentionFront(const SyntaxRevisionKey& key)
{
    if (key.generation != 0)
    {
        return m_entries.begin();
    }
    return std::ranges::find_if(m_entries, [](const Entry& entry) { return entry.generation == 0; });
}

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
    m_entries.splice(RetentionFront(key), m_entries, found);
    return found->syntax;
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
    m_entries.insert(RetentionFront(key),
                     {std::string(key.uri), std::string(key.source), key.generation, key.closedRevision, syntax});

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

SyntaxRevision ResolveSyntaxRevision(SyntaxRevisionInput input, SyntaxRevisionCache* cache)
{
    if (cache)
    {
        if (auto existing = cache->Find(input.key))
        {
            return {std::move(existing)};
        }
    }

    utils::HighResTimer parseTimer;
    if (!input.tree)
    {
        input.tree = parser::MakeTreePtr(input.exactTreeToCopy ? ts_tree_copy(input.exactTreeToCopy)
                                                               : input.parser.Parse(input.key.source));
    }
    const double parseMs = parseTimer.ElapsedMs();
    if (!input.tree)
    {
        return {.parseMs = parseMs};
    }

    auto syntax = std::make_shared<const ParsedDocumentSyntax>(std::move(input.tree), input.key.source);
    const size_t extractedNodes = syntax->index.NodeCount();
    if (cache)
    {
        cache->Remember(input.key, syntax);
    }
    return {std::move(syntax), extractedNodes, parseMs};
}
} // namespace angel_lsp
