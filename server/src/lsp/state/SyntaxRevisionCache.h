#pragma once

#include "analysis/index/NodeIndex.h"
#include "document/Document.h"
#include "parser/AngelScriptParser.h"

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
    double parseMs = 0.0;
};

/**
 * @brief Retains exact syntax revisions under shared budgets, with open revisions retained first.
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

    /// Inserts or promotes within the open or closed partition.
    std::list<Entry>::iterator RetentionFront(const SyntaxRevisionKey& key);

    static constexpr size_t MaxEntries = 8;
    static constexpr size_t MaxNodes = 32768;
    static constexpr size_t MaxSourceBytes = 2 * 1024 * 1024;
    std::list<Entry> m_entries;
};
/// Inputs owned by the caller or borrowed for the duration of syntax resolution.
struct SyntaxRevisionInput
{
    SyntaxRevisionKey key;
    parser::AngelScriptParser& parser;
    document::TreePtr tree = document::MakeTreePtr();
    const TSTree* exactTreeToCopy = nullptr;
};

/**
 * @brief Resolves syntax on the caller's thread, preferring an existing extraction.
 * @param[in] input Revision key, parser and optional tree for the exact same parser input.
 * @param[in,out] cache Optional cache owned by the calling thread.
 * @return The exact syntax revision and the work performed to produce it.
 */
[[nodiscard]] SyntaxRevision ResolveSyntaxRevision(SyntaxRevisionInput input, SyntaxRevisionCache* cache = nullptr);
} // namespace angel_lsp
