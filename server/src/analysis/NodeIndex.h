#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Single-pass index of AST nodes grouped by Tree-Sitter symbol for O(1) retrieval.
     *
     * Traverses an AST once in preorder, bucketing nodes by their Tree-Sitter TSSymbol id
     * and storing a flat preorder list of all nodes. Checkers query this index instead of
     * recursively traversing the full AST tree multiple times per document analysis.
     */
    class NodeIndex
    {
    public:
        NodeIndex() = default;

        /**
         * @brief Constructs a NodeIndex and builds it from the given root node.
         * @param root The root AST node of the document.
         * @param lang The language definition (defaults to tree_sitter_angelscript() if null).
         */
        explicit NodeIndex(TSNode root, const TSLanguage *lang = nullptr);

        ~NodeIndex() = default;
        NodeIndex(const NodeIndex &) = default;
        NodeIndex &operator=(const NodeIndex &) = default;
        NodeIndex(NodeIndex &&) noexcept = default;
        NodeIndex &operator=(NodeIndex &&) noexcept = default;

        /**
         * @brief Builds or rebuilds the index from an AST root node.
         * @param root The root node of the syntax tree.
         * @param lang The language definition (defaults to tree_sitter_angelscript() if null).
         */
        void Build(TSNode root, const TSLanguage *lang = nullptr);

        /**
         * @brief Returns all nodes of a given grammar symbol id in document (preorder) order.
         * @param symbol The Tree-Sitter TSSymbol id.
         * @return Span of matching nodes. Empty if symbol is out of bounds or has no occurrences.
         */
        [[nodiscard]] std::span<const TSNode> Nodes(TSSymbol symbol) const noexcept;

        /**
         * @brief Returns all nodes of a given node type name in document order.
         * @param typeName The grammar node type name (e.g. parser::nodes::CallExpression).
         * @return Span of matching nodes. Empty if type name has no occurrences.
         */
        [[nodiscard]] std::span<const TSNode> Nodes(std::string_view typeName) const noexcept;

        /**
         * @brief Resolves a node type name to its TSSymbol id.
         * @param typeName The grammar node type name.
         * @return The TSSymbol id, or 0 if unknown.
         */
        [[nodiscard]] TSSymbol SymbolForName(std::string_view typeName) const noexcept;

        /**
         * @brief Returns all indexed nodes in document (preorder) order.
         */
        [[nodiscard]] std::span<const TSNode> AllNodes() const noexcept;

        /**
         * @brief Returns the root node the index was built from.
         */
        [[nodiscard]] TSNode Root() const noexcept;

        /**
         * @brief Returns the total count of indexed nodes.
         */
        [[nodiscard]] size_t NodeCount() const noexcept;

        /**
         * @brief True if index is empty.
         */
        [[nodiscard]] bool Empty() const noexcept;

        /**
         * @brief Clears all indexed nodes and symbol maps.
         */
        void Clear() noexcept;

    private:
        TSNode m_root{};
        const TSLanguage *m_language = nullptr;
        std::vector<TSNode> m_allNodes;
        std::vector<std::vector<TSNode>> m_nodesBySymbol;
        ankerl::unordered_dense::map<std::string_view, TSSymbol> m_symbolByName;
    };
}
