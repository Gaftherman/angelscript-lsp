#pragma once

#include "analysis/SymbolTable.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
class TraversalBudget;

/**
 * @brief Information about an enclosing container in the AST hierarchy.
 */
struct ContainerInfo
{
    std::string name;          ///< Bare name, e.g. "Player" or "Game"
    std::string qualifiedName; ///< Fully qualified path, e.g. "Game::Player" or "Game"
    ContainerKind kind;
};

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
     * @brief Iterates multiple grammar symbols in document order (sorted by start byte).
     * @tparam Callback Visitor callable with signature void(TSNode).
     * @param[in] symbols List of Tree-Sitter grammar symbol IDs to merge.
     * @param[in] callback Visitor invoked for each node in sorted order.
     */
    template <typename Callback> void ForEachNodeOrdered(std::span<const TSSymbol> symbols, Callback&& callback) const
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
        cursors.reserve(symbols.size());
        for (TSSymbol sym : symbols)
        {
            cursors.push_back(Cursor{Nodes(sym), 0});
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
     * @brief Represents a half-open byte range [startByte, endByte) containing a syntax error.
     */
    struct ErrorRange
    {
        uint32_t startByte = 0;
        uint32_t endByte = 0;
    };

    /**
     * @brief Represents a half-open byte range [startByte, endByte) containing a comment node.
     */
    struct CommentRange
    {
        uint32_t startByte = 0;
        uint32_t endByte = 0;
        uint32_t startLine = 0;
        uint32_t startColumn = 0;
        uint32_t endLine = 0;
        uint32_t endColumn = 0;
    };

    /**
     * @brief Checks in O(log E) whether the given byte offset falls inside any syntax error node.
     * @param[in] byteOffset Byte offset to test.
     * @return True if offset is inside a syntax error range; false otherwise (O(1) if no errors).
     */
    [[nodiscard]] bool IsInsideError(uint32_t byteOffset) const noexcept;

    /**
     * @brief Checks in O(log C) whether the given byte offset falls inside any comment node.
     * @param[in] byteOffset Byte offset to test.
     * @return True if offset is inside a comment range; false otherwise (O(1) if no comments).
     */
    [[nodiscard]] bool IsInsideComment(uint32_t byteOffset) const noexcept;

    /**
     * @brief Checks in O(log C) whether the given line and column fall inside any comment node.
     * @param[in] line 0-based line index.
     * @param[in] col 0-based column index.
     * @return True if position is inside a comment range; false otherwise.
     */
    [[nodiscard]] bool IsInsideCommentLineCol(uint32_t line, uint32_t col) const noexcept;

    /**
     * @brief Checks whether the given node is an error, has errors, or is enclosed in a syntax error.
     * @param[in] node AST node to test.
     * @return True if node is or is inside an error; false otherwise.
     */
    [[nodiscard]] bool NodeHasSyntaxError(TSNode node) const noexcept;

    /**
     * @brief Returns all tracked disjoint syntax error byte ranges.
     * @return Span of error ranges.
     */
    [[nodiscard]] std::span<const ErrorRange> ErrorRanges() const noexcept;

    /**
     * @brief Returns all tracked disjoint comment byte ranges.
     * @return Span of comment ranges.
     */
    [[nodiscard]] std::span<const CommentRange> CommentRanges() const noexcept;

    /**
     * @brief Finds the innermost class, interface, or mixin declaration enclosing the given byte offset.
     * @param[in] byteOffset Document byte offset to test.
     * @return Innermost enclosing class/interface/mixin TSNode or null node if outside any class.
     */
    [[nodiscard]] TSNode FindEnclosingClassOrMixin(uint32_t byteOffset) const noexcept;

    /**
     * @brief Finds the innermost callable declaration (function or lambda expression) enclosing byteOffset.
     * @param[in] byteOffset Document byte offset to test.
     * @return Innermost enclosing function or lambda TSNode or null node if none found.
     */
    [[nodiscard]] TSNode FindEnclosingCallable(uint32_t byteOffset) const noexcept;

    /**
     * @brief Tests whether byteOffset is enclosed by an intermediate lambda expression closure barrier.
     * @param[in] byteOffset Document byte offset to test.
     * @return True if the innermost callable enclosing byteOffset is a lambda expression.
     */
    [[nodiscard]] bool IsInsideLambdaClosure(uint32_t byteOffset) const noexcept;

    /**
     * @brief Reconstructs the enclosing container hierarchy (namespaces, classes, interfaces) for byteOffset.
     *        Operates in O(K) where K is the number of containers in the document, with zero GetParent calls.
     * @param[in] byteOffset Document byte offset to test.
     * @param[in] sourceCode Document source text buffer.
     * @return Vector of ContainerInfo ordered from innermost to outermost container.
     */
    [[nodiscard]] std::vector<ContainerInfo> CollectEnclosingContainers(uint32_t byteOffset,
                                                                        std::string_view sourceCode) const;

    /**
     * @brief Clears all indexed nodes and symbol maps.
     */
    void Clear() noexcept;

  private:
    void PopulatePredefinedSymbols(const TSLanguage* lang);
    void IndexNode(TSNode node, TraversalBudget* budget = nullptr);
    void TraverseTree(TSNode root, TraversalBudget* budget = nullptr);
    void PostProcessErrorRanges();
    void PostProcessCommentRanges();

    TSNode m_root{};
    const TSLanguage* m_language = nullptr;
    std::vector<TSNode> m_allNodes;
    std::vector<TSNode> m_errorNodes;
    std::vector<std::vector<TSNode>> m_nodesBySymbol;
    std::vector<ErrorRange> m_errorRanges;
    std::vector<CommentRange> m_commentRanges;
    ankerl::unordered_dense::map<std::string_view, TSSymbol> m_symbolByName;
    TSSymbol m_symComment = 0;
};
} // namespace angel_lsp::analysis
