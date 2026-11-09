#pragma once

#include "analysis/index/NodeIndex.h"
#include "parser/AngelScriptParser.h"
#include "parser/SyntaxTree.h"

#include <ankerl/unordered_dense.h>
#include <memory>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Cache entry for parsed mixin source and AST tree.
 */
struct MixinFileCacheEntry
{
    /// Owned mixin document text that tree and nodeIndex borrow from.
    std::string source;
    /// Parser that produced tree.
    std::unique_ptr<parser::AngelScriptParser> parser;
    /// Owned syntax tree.
    parser::TreePtr tree;
    /// First-pass index over tree.
    std::unique_ptr<NodeIndex> nodeIndex;
};

/// Mixin parse cache keyed by document URI, reused across class validations within one analysis run.
using MixinFileCache = ankerl::unordered_dense::map<std::string, MixinFileCacheEntry>;

/**
 * @brief Parses mixin source text with a fresh parser and constructs a cached AST entry.
 * @param[in] source Mixin source code buffer to parse.
 * @return Populated MixinFileCacheEntry containing the parsed tree and node index, or an empty entry if source is
 * empty.
 */
[[nodiscard]] MixinFileCacheEntry BuildMixinFileCacheEntry(std::string source);

/**
 * @brief Copies already loaded document text and parses it into a mixin cache entry.
 * @param[in] text Document text, typically the open buffer of the current request; copied into the entry.
 * @return Entry built by BuildMixinFileCacheEntry, or an entry with empty source when text is empty.
 */
[[nodiscard]] MixinFileCacheEntry BuildMixinFileCacheEntryFromText(std::string_view text);

/**
 * @brief Reads a mixin document from disk and parses it into a cache entry.
 * @param[in] fileUri file:// URI of the document declaring the mixin.
 * @return Entry built by BuildMixinFileCacheEntry, or an entry with empty source when the file cannot be opened
 *         or is empty.
 */
[[nodiscard]] MixinFileCacheEntry LoadMixinFileCacheEntry(std::string_view fileUri);

/**
 * @brief Constructs a first-pass NodeIndex over the root of a syntax tree.
 * @param[in] tree Pointer to the Tree-Sitter syntax tree to index.
 * @return Owning pointer to the created NodeIndex, or nullptr when tree is null or root is invalid.
 */
[[nodiscard]] std::unique_ptr<NodeIndex> BuildNodeIndexForTree(const TSTree* tree);

} // namespace angel_lsp::analysis
