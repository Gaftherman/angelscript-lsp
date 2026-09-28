#pragma once

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
class TraversalBudget;

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
     * @param budget Optional traversal budget to track and enforce node visits.
     */
    explicit NodeIndex(TSNode root, const TSLanguage* lang = nullptr, TraversalBudget* budget = nullptr);

    ~NodeIndex() = default;
    NodeIndex(const NodeIndex&) = default;
    NodeIndex& operator=(const NodeIndex&) = default;
    NodeIndex(NodeIndex&&) noexcept = default;
    NodeIndex& operator=(NodeIndex&&) noexcept = default;

    /**
     * @brief Builds or rebuilds the index from an AST root node.
     * @param root The root node of the syntax tree.
     * @param lang The language definition (defaults to tree_sitter_angelscript() if null).
     * @param budget Optional traversal budget to track and enforce node visits.
     */
    void Build(TSNode root, const TSLanguage* lang = nullptr, TraversalBudget* budget = nullptr);

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
     * @brief Iterates multiple node types in document order (sorted by start byte).
     * @tparam Callback Visitor callable with signature void(TSNode).
     * @param[in] types List of Tree-Sitter node type names to merge.
     * @param[in] callback Visitor invoked for each node in sorted order.
     */
    template <typename Callback>
    void ForEachNodeOrdered(std::span<const std::string_view> types, Callback&& callback) const
    {
        struct Cursor
        {
            std::span<const TSNode> nodes;
            size_t index = 0;
            [[nodiscard]] uint32_t currentByte() const noexcept
            {
                return (index < nodes.size()) ? ts_node_start_byte(nodes[index]) : UINT32_MAX;
            }
        };

        std::vector<Cursor> cursors;
        cursors.reserve(types.size());
        for (std::string_view typeName : types)
        {
            cursors.push_back(Cursor{Nodes(typeName), 0});
        }

        while (true)
        {
            size_t best = 0;
            uint32_t minByte = UINT32_MAX;
            for (size_t c = 0; c < cursors.size(); ++c)
            {
                uint32_t b = cursors[c].currentByte();
                if (b < minByte)
                {
                    minByte = b;
                    best = c;
                }
            }
            if (minByte == UINT32_MAX)
            {
                break;
            }

            TSNode node = cursors[best].nodes[cursors[best].index++];
            callback(node);
        }
    }

    /**
     * @brief Iterates over multiple node types in document order (initializer_list overload).
     */
    template <typename Callback>
    void ForEachNodeOrdered(std::initializer_list<std::string_view> types, Callback&& callback) const
    {
        ForEachNodeOrdered(std::span<const std::string_view>(types.begin(), types.size()),
                           std::forward<Callback>(callback));
    }

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
    void PopulatePredefinedSymbols(const TSLanguage* lang);
    void IndexNode(TSNode node, TraversalBudget* budget = nullptr);
    void TraverseTree(TSNode root, TraversalBudget* budget = nullptr);

    TSNode m_root{};
    const TSLanguage* m_language = nullptr;
    std::vector<TSNode> m_allNodes;
    std::vector<std::vector<TSNode>> m_nodesBySymbol;
    ankerl::unordered_dense::map<std::string_view, TSSymbol> m_symbolByName;
};
} // namespace angel_lsp::analysis
