#pragma once

#include "analysis/index/ConstQualifierFacts.h"
#include "analysis/index/LocalConstFacts.h"
#include "analysis/index/NodeIndex.h"
#include "document/Document.h"
#include "parser/AngelScriptParser.h"
#include "parser/SyntaxTree.h"

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
    explicit ParsedDocumentSyntax(parser::TreePtr parsed, std::string_view source = {})
        : tree(std::move(parsed)), index(parser::GetRootNode(tree), analysis::NodeIndexOptions{.source = source}),
          parameterRefFacts(analysis::BuildParameterRefFacts(index, source)), localConstFacts(index)
    {
    }

    /**
     * @brief Estimates retained bytes for the tree, index, and caller-owned source text.
     * @param[in] sourceBytes Length in bytes of the retained source string.
     * @return Estimated memory footprint in bytes.
     * @note The tree share comes from NodeIndex::EstimateTreeBytes, a conservative per-node bound.
     */
    [[nodiscard]] size_t EstimateMemoryBytes(size_t sourceBytes = 0) const noexcept
    {
        return index.EstimateMemoryBytes() + sourceBytes + (tree ? index.EstimateTreeBytes() : 0) +
               sizeof(parameterRefFacts) +
               parameterRefFacts.sites.capacity() * sizeof(analysis::IndexedParameterRefSite) +
               localConstFacts.EstimateMemoryBytes();
    }

    parser::TreePtr tree;
    analysis::NodeIndex index;
    analysis::ParameterRefFacts parameterRefFacts; ///< Reference-direction edits for the retained syntax.
    analysis::LocalConstFacts localConstFacts;     ///< Declaration and escape facts for the retained syntax.
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
    static constexpr size_t MaxEntries = 8;
    static constexpr size_t MaxRetainedBytes = size_t{96} * 1024 * 1024;
    /// Raised to 8 MiB per revision so large AngelScript modules (>20k lines) remain cacheable without re-parsing.
    static constexpr size_t MaxSourceBytes = size_t{8} * 1024 * 1024;

    /**
     * @brief Constructs a revision cache bounded by entry count, total retained bytes, and per-entry source size.
     * @param[in] maxEntries Maximum number of cached revisions.
     * @param[in] maxRetainedBytes Total estimated byte budget across all retained revisions.
     * @param[in] maxSourceBytes Maximum source size in bytes for a single cacheable revision.
     */
    explicit SyntaxRevisionCache(size_t maxEntries = MaxEntries, size_t maxRetainedBytes = MaxRetainedBytes,
                                 size_t maxSourceBytes = MaxSourceBytes) noexcept
        : m_maxEntries(maxEntries), m_maxRetainedBytes(maxRetainedBytes), m_maxSourceBytes(maxSourceBytes)
    {
    }

    /// Finds an exact revision and promotes it; an unknown revision is never cached.
    [[nodiscard]] std::shared_ptr<const ParsedDocumentSyntax> Find(const SyntaxRevisionKey& key);

    /// Keeps the most recent eligible extraction for a document within the retention budgets.
    void Remember(const SyntaxRevisionKey& key, const std::shared_ptr<const ParsedDocumentSyntax>& syntax);

    /// Releases the retained syntax when the protocol owner closes the document.
    void Forget(std::string_view uri);

    /// Returns the total estimated memory footprint of all currently retained revisions in bytes.
    [[nodiscard]] size_t RetainedBytes() const noexcept;

    /// Returns the number of currently retained revisions.
    [[nodiscard]] size_t EntryCount() const noexcept
    {
        return m_entries.size();
    }

  private:
    struct Entry
    {
        std::string uri;
        std::string source;
        uint64_t generation;
        uint64_t closedRevision;
        std::shared_ptr<const ParsedDocumentSyntax> syntax;
        size_t estimatedBytes = 0;
    };

    /// Inserts or promotes within the open or closed partition.
    std::list<Entry>::iterator RetentionFront(const SyntaxRevisionKey& key);

    size_t m_maxEntries = MaxEntries;
    size_t m_maxRetainedBytes = MaxRetainedBytes;
    size_t m_maxSourceBytes = MaxSourceBytes;
    std::list<Entry> m_entries;
};
/// Inputs owned by the caller or borrowed for the duration of syntax resolution.
struct SyntaxRevisionInput
{
    SyntaxRevisionKey key;
    parser::AngelScriptParser& parser;
    parser::TreePtr tree = parser::MakeTreePtr();
    const parser::TreePtr* exactTreeToCopy = nullptr;
};

/**
 * @brief Resolves syntax on the caller's thread, preferring an existing extraction.
 * @param[in] input Revision key, parser and optional tree for the exact same parser input.
 * @param[in,out] cache Optional cache owned by the calling thread.
 * @return The exact syntax revision and the work performed to produce it.
 */
[[nodiscard]] SyntaxRevision ResolveSyntaxRevision(SyntaxRevisionInput input, SyntaxRevisionCache* cache = nullptr);
} // namespace angel_lsp
