#pragma once

#include <memory>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{

/**
 * @brief Canonical owning handle of a parsed Tree-Sitter syntax tree, released through ts_tree_delete.
 */
using TreePtr = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

/**
 * @brief Adopts a raw Tree-Sitter parse tree into a managed TreePtr owning handle.
 * @param[in] tree Raw tree to adopt, may be null.
 * @return An owning TreePtr managing the supplied tree, or an empty TreePtr if tree is null.
 */
[[nodiscard]] inline TreePtr MakeTreePtr(TSTree* tree = nullptr)
{
    return TreePtr(tree, &ts_tree_delete);
}

/**
 * @brief Creates an independent, thread-safe owning copy of a Tree-Sitter syntax tree.
 * @param[in] tree Tree to copy, may be null.
 * @return An owning TreePtr managing the newly duplicated tree, or an empty TreePtr when tree is null.
 * @note The underlying ts_tree_copy function performs a cheap shallow copy through reference counting
 *       and is the only safe way to hand a syntax tree to another thread for concurrent access.
 */
[[nodiscard]] inline TreePtr CopyTree(const TSTree* tree)
{
    return MakeTreePtr(tree ? ts_tree_copy(tree) : nullptr);
}

} // namespace angel_lsp::parser
