#pragma once

#include <string>
#include <memory>
#include <utility>
#include <tree_sitter/api.h>

namespace angel_lsp::document
{
    /**
     * @brief Custom deleter for Tree-sitter ASTs using ts_tree_delete.
     */
    using TreePtr = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

    /**
     * @brief Creates a managed TreePtr for an existing or newly parsed TSTree.
     */
    inline TreePtr MakeTreePtr(TSTree *tree = nullptr)
    {
        return TreePtr(tree, &ts_tree_delete);
    }

    /**
     * @brief Represents an in-memory document managed by the language server.
     */
    struct Document
    {
        std::string uri;
        std::string text;
        int version = 0;
        TreePtr tree = MakeTreePtr(nullptr);

        Document() = default;
        Document(std::string u, std::string t, int v = 0, TSTree *tr = nullptr)
            : uri(std::move(u)), text(std::move(t)), version(v), tree(MakeTreePtr(tr))
        {
        }
        Document(std::string u, std::string t, int v, TreePtr tr)
            : uri(std::move(u)), text(std::move(t)), version(v), tree(std::move(tr))
        {
        }
    };
}
