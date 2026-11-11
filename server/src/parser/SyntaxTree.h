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

/**
 * @brief Creates an independent, thread-safe owning copy of a managed syntax tree.
 * @param[in] tree Owning tree pointer to copy.
 * @return An owning TreePtr managing the newly duplicated tree, or an empty TreePtr when tree is empty.
 */
[[nodiscard]] inline TreePtr CopyTree(const TreePtr& tree)
{
    return CopyTree(tree.get());
}

/**
 * @brief Retrieves root node of a managed TreePtr syntax tree.
 * @param[in] tree Owning tree pointer.
 * @return Root TSNode or null node if tree is null.
 */
[[nodiscard]] inline TSNode GetRootNode(const TreePtr& tree) noexcept
{
    return tree ? ts_tree_root_node(tree.get()) : TSNode{};
}

/**
 * @brief Checks whether a managed syntax tree has any syntax errors.
 * @param[in] tree Owning tree pointer.
 * @return True if syntax tree root contains errors, false otherwise.
 */
[[nodiscard]] inline bool TreeHasError(const TreePtr& tree) noexcept
{
    return tree && ts_node_has_error(ts_tree_root_node(tree.get()));
}

/**
 * @brief Abstract text change geometry for Tree-Sitter incremental edits.
 */
struct TextChangeEdit
{
    uint32_t startByte = 0;
    uint32_t oldEndByte = 0;
    uint32_t newEndByte = 0;
    uint32_t startRow = 0;
    uint32_t startColumn = 0;
    uint32_t oldEndRow = 0;
    uint32_t oldEndColumn = 0;
    uint32_t newEndRow = 0;
    uint32_t newEndColumn = 0;
};

/**
 * @brief Applies an incremental edit to a managed syntax tree.
 * @param[in,out] tree Owning tree to edit.
 * @param[in] edit Text change edit geometry.
 */
inline void ApplyTreeEdit(const TreePtr& tree, const TextChangeEdit& edit) noexcept
{
    if (!tree)
    {
        return;
    }
    TSInputEdit tsEdit;
    tsEdit.start_byte = edit.startByte;
    tsEdit.old_end_byte = edit.oldEndByte;
    tsEdit.new_end_byte = edit.newEndByte;
    tsEdit.start_point = TSPoint{edit.startRow, edit.startColumn};
    tsEdit.old_end_point = TSPoint{edit.oldEndRow, edit.oldEndColumn};
    tsEdit.new_end_point = TSPoint{edit.newEndRow, edit.newEndColumn};
    ts_tree_edit(tree.get(), &tsEdit);
}

} // namespace angel_lsp::parser
