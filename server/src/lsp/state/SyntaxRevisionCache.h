#pragma once

#include "analysis/index/NodeIndex.h"
#include "document/Document.h"

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <string_view>

namespace angel_lsp
{
/** @brief Owns the tree backing an extracted index on the producing thread. */
struct ParsedDocumentSyntax
{
    explicit ParsedDocumentSyntax(document::TreePtr parsed)
        : tree(std::move(parsed)), index(tree ? ts_tree_root_node(tree.get()) : TSNode{})
    {
    }

    document::TreePtr tree;
    analysis::NodeIndex index;
};

/// Identifies exact parser input for an open generation or a closed disk revision.
struct SyntaxRevisionKey
{
    std::string_view uri;
    std::string_view source;
    uint64_t generation = 0;
    uint64_t closedRevision = 0;
};

/// Syntax available to a caller and the number of nodes extracted to obtain it.
struct SyntaxRevision
{
    std::shared_ptr<const ParsedDocumentSyntax> syntax;
    size_t extractedNodes = 0;
};

/**
 * @brief Retains a bounded set of exact syntax revisions in least-recently-used order.
 * @note Each instance and its syntax handles belong to a single execution thread.
 */
class SyntaxRevisionCache
{
  public:
    /// Finds an exact revision and promotes it; an unknown revision is never cached.
    [[nodiscard]] std::shared_ptr<const ParsedDocumentSyntax> Find(const SyntaxRevisionKey& key);

    /// Keeps the most recent eligible extraction for a document within the retention budgets.
    void Remember(const SyntaxRevisionKey& key, const std::shared_ptr<const ParsedDocumentSyntax>& syntax);

    /// Releases the retained syntax when the protocol owner closes the document.
    void Forget(std::string_view uri);

  private:
    struct Entry
    {
        std::string uri;
        std::string source;
        uint64_t generation;
        uint64_t closedRevision;
        std::shared_ptr<const ParsedDocumentSyntax> syntax;
    };

    static constexpr size_t MaxEntries = 8;
    static constexpr size_t MaxNodes = 32768;
    static constexpr size_t MaxSourceBytes = 2 * 1024 * 1024;
    std::list<Entry> m_entries;
};
} // namespace angel_lsp
