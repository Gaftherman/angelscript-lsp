#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>

namespace angel_lsp
{
/**
 * @brief Custom deleter for Tree-sitter ASTs using ts_tree_delete.
 */
struct TSTreeDeleter
{
    void operator()(TSTree* tree) const noexcept
    {
        if (tree)
        {
            ts_tree_delete(tree);
        }
    }
};

using SharedTree = std::shared_ptr<const TSTree>;
} // namespace angel_lsp

namespace angel_lsp::document
{
/**
 * @brief Custom deleter for Tree-sitter ASTs using ts_tree_delete.
 */
using TreePtr = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

/**
 * @brief Creates a managed TreePtr for an existing or newly parsed TSTree.
 */
inline TreePtr MakeTreePtr(TSTree* tree = nullptr)
{
    return TreePtr(tree, &ts_tree_delete);
}

/**
 * @brief Snapshot parameters for constructing a Document.
 */
struct DocumentSnapshot
{
    std::string uri;
    std::string text;
    int version = 0;
    uint64_t generation = 0;
    std::shared_ptr<const std::string> transformedSource = {};
};

/**
 * @brief In-memory document managed by the language server.
 */
struct Document
{
    std::string uri;
    std::string text;
    int version = 0;
    uint64_t generation = 0;
    TreePtr tree = MakeTreePtr(nullptr);

    Document() = default;

    Document(std::string u, std::string t, int v = 0, TSTree* tr = nullptr)
        : uri(std::move(u)), text(std::move(t)), version(v), generation(0), tree(MakeTreePtr(tr)),
          astTree_(tr ? SharedTree(ts_tree_copy(tr), TSTreeDeleter{}) : nullptr)
    {
    }

    Document(std::string u, std::string t, int v, TreePtr tr)
        : uri(std::move(u)), text(std::move(t)), version(v), generation(0), tree(std::move(tr)),
          astTree_(tree ? SharedTree(ts_tree_copy(tree.get()), TSTreeDeleter{}) : nullptr)
    {
    }

    Document(DocumentSnapshot snapshot, TreePtr tr)
        : uri(std::move(snapshot.uri)), text(std::move(snapshot.text)), version(snapshot.version),
          generation(snapshot.generation), tree(std::move(tr)),
          transformedSource_(std::move(snapshot.transformedSource)),
          astTree_(tree ? SharedTree(ts_tree_copy(tree.get()), TSTreeDeleter{}) : nullptr)
    {
    }

    Document(const Document& other)
        : uri(other.uri), text(other.text), version(other.version), generation(other.generation),
          tree(other.tree ? MakeTreePtr(ts_tree_copy(other.tree.get())) : MakeTreePtr(nullptr)),
          transformedSource_(other.transformedSource_), astTree_(other.getAST())
    {
    }

    Document(Document&& other) noexcept
        : uri(std::move(other.uri)), text(std::move(other.text)), version(other.version), generation(other.generation),
          tree(std::move(other.tree)), transformedSource_(std::move(other.transformedSource_)),
          astTree_(std::move(other.astTree_))
    {
    }

    Document& operator=(const Document& other)
    {
        if (this != &other)
        {
            uri = other.uri;
            text = other.text;
            version = other.version;
            generation = other.generation;
            tree = other.tree ? MakeTreePtr(ts_tree_copy(other.tree.get())) : MakeTreePtr(nullptr);
            transformedSource_ = other.transformedSource_;
            astTree_ = other.getAST();
        }
        return *this;
    }

    Document& operator=(Document&& other) noexcept
    {
        if (this != &other)
        {
            uri = std::move(other.uri);
            text = std::move(other.text);
            version = other.version;
            generation = other.generation;
            tree = std::move(other.tree);
            transformedSource_ = std::move(other.transformedSource_);
            astTree_ = std::move(other.astTree_);
        }
        return *this;
    }

    /// Returns the exact bytes parsed into the document tree.
    [[nodiscard]] std::string_view ParsedSource() const noexcept
    {
        return transformedSource_ ? std::string_view(*transformedSource_) : std::string_view(text);
    }

    [[nodiscard]] const std::shared_ptr<const std::string>& TransformedSource() const noexcept
    {
        return transformedSource_;
    }

    /**
     * @brief Gets an immutable snapshot handle of the parsed AST.
     * @return SharedTree immutable pointer to TSTree.
     */
    [[nodiscard]] SharedTree getAST() const
    {
        std::shared_lock<std::shared_mutex> lock(treeMutex_);
        if (astTree_)
        {
            return astTree_;
        }
        if (tree)
        {
            return SharedTree(ts_tree_copy(tree.get()), TSTreeDeleter{});
        }
        return nullptr;
    }

    /**
     * @brief Sets the parsed AST taking ownership of a raw pointer.
     * @param[in] rawTree Raw TSTree pointer to take ownership of.
     */
    void setAST(TSTree* rawTree)
    {
        SharedTree newTree(rawTree, TSTreeDeleter{});
        std::unique_lock<std::shared_mutex> lock(treeMutex_);
        astTree_ = std::move(newTree);
    }

    /**
     * @brief Sets the parsed AST from an existing SharedTree handle.
     * @param[in] newTree SharedTree snapshot handle.
     */
    void setAST(SharedTree newTree)
    {
        std::unique_lock<std::shared_mutex> lock(treeMutex_);
        astTree_ = std::move(newTree);
    }

  private:
    std::shared_ptr<const std::string> transformedSource_;
    mutable std::shared_mutex treeMutex_;
    SharedTree astTree_;
};
} // namespace angel_lsp::document
