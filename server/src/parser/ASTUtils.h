#pragma once

#include "parser/GrammarSymbols.h"
#include "utils/Constants.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{
/**
 * @brief Returns the AST node type name as a zero-allocation string view.
 * @param[in] node The TSNode to inspect.
 * @return String view containing the node type name, or empty if null.
 */
[[nodiscard]] inline std::string_view NodeType(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const char* t = ts_node_type(node);
    return t ? std::string_view(t) : std::string_view{};
}

[[nodiscard]] inline std::string_view GetNodeType(TSNode node) noexcept
{
    return NodeType(node);
}

/**
 * @brief Returns the root AST node of the document containing the given node in O(1).
 * @param[in] node Any AST node within a parsed tree.
 * @return Root TSNode of the tree, or a null node if node is null or unassociated.
 */
[[nodiscard]] inline TSNode GetRootNode(TSNode node) noexcept
{
    return (ts_node_is_null(node) || node.tree == nullptr) ? TSNode{} : ts_tree_root_node(node.tree);
}

/**
 * @brief Returns the immediate parent AST node safely via ts_node_parent.
 * @param[in] node Child AST node.
 * @return Parent AST node or null node if node is null or at root.
 */
[[nodiscard]] inline TSNode GetParent(TSNode node) noexcept
{
    return ts_node_is_null(node) ? TSNode{} : ts_node_parent(node);
}

/**
 * @brief Convertible string slice of an AST node that converts to both std::string_view and std::string.
 *
 * Resolves the conversion barrier where callers needing an owning std::string had to either
 * explicitly construct it or define duplicate translation-unit local functions.
 */
struct NodeTextResult
{
    std::string_view view{};

    constexpr NodeTextResult() noexcept = default;
    constexpr NodeTextResult(std::string_view v) noexcept : view(v)
    {
    }

    constexpr operator std::string_view() const noexcept
    {
        return view;
    }
    operator std::string() const
    {
        return std::string(view);
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return view.empty();
    }
    [[nodiscard]] constexpr size_t size() const noexcept
    {
        return view.size();
    }
    [[nodiscard]] constexpr size_t length() const noexcept
    {
        return view.length();
    }
    [[nodiscard]] constexpr const char* data() const noexcept
    {
        return view.data();
    }
    [[nodiscard]] constexpr char front() const
    {
        return view.front();
    }
    [[nodiscard]] constexpr char back() const
    {
        return view.back();
    }
    [[nodiscard]] constexpr char operator[](size_t i) const
    {
        return view[i];
    }

    static constexpr size_t npos = std::string_view::npos;
    [[nodiscard]] size_t find(std::string_view s, size_t pos = 0) const noexcept
    {
        return view.find(s, pos);
    }
    [[nodiscard]] size_t find(char c, size_t pos = 0) const noexcept
    {
        return view.find(c, pos);
    }
    [[nodiscard]] bool starts_with(std::string_view s) const noexcept
    {
        return view.starts_with(s);
    }
    [[nodiscard]] bool ends_with(std::string_view s) const noexcept
    {
        return view.ends_with(s);
    }
    [[nodiscard]] NodeTextResult substr(size_t pos = 0, size_t count = std::string_view::npos) const noexcept
    {
        return NodeTextResult(view.substr(pos, count));
    }

    friend bool operator==(const NodeTextResult& lhs, std::string_view rhs) noexcept
    {
        return lhs.view == rhs;
    }
    friend bool operator==(std::string_view lhs, const NodeTextResult& rhs) noexcept
    {
        return lhs == rhs.view;
    }
    friend bool operator==(const NodeTextResult& lhs, const NodeTextResult& rhs) noexcept
    {
        return lhs.view == rhs.view;
    }
    friend bool operator!=(const NodeTextResult& lhs, std::string_view rhs) noexcept
    {
        return lhs.view != rhs;
    }
    friend bool operator!=(std::string_view lhs, const NodeTextResult& rhs) noexcept
    {
        return lhs != rhs.view;
    }
    friend bool operator!=(const NodeTextResult& lhs, const NodeTextResult& rhs) noexcept
    {
        return lhs.view != rhs.view;
    }

    friend std::string operator+(const std::string& lhs, const NodeTextResult& rhs)
    {
        return lhs + std::string(rhs.view);
    }
    friend std::string operator+(const NodeTextResult& lhs, const std::string& rhs)
    {
        return std::string(lhs.view) + rhs;
    }
};

/**
 * @brief Extracts the raw source text corresponding to an AST node.
 * @param[in] node The TSNode whose slice to extract.
 * @param[in] sourceCode The full document source code.
 * @return Convertible slice of sourceCode spanning the node's byte range.
 */
[[nodiscard]] inline NodeTextResult NodeText(TSNode node, std::string_view sourceCode) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= sourceCode.size() || end > sourceCode.size() || start >= end)
    {
        return {};
    }
    return NodeTextResult(sourceCode.substr(start, end - start));
}

/**
 * @brief Extracts the raw source text corresponding to an AST node as an owned string.
 * @param[in] node The TSNode whose slice to extract.
 * @param[in] sourceCode The full document source code.
 * @return std::string spanning the node's byte range.
 */
[[nodiscard]] inline std::string NodeTextString(TSNode node, std::string_view sourceCode)
{
    return std::string(NodeText(node, sourceCode));
}

/**
 * @brief Extracts source code substring corresponding to an AST node.
 * @param[in] node AST node.
 * @param[in] sourceCode Document source text.
 * @return Extracted string, or empty string if node is null or out of bounds.
 */
[[nodiscard]] inline std::string GetNodeText(TSNode node, std::string_view sourceCode)
{
    return NodeTextString(node, sourceCode);
}

/**
 * @brief Extracts source code substring view corresponding to an AST node.
 * @param[in] node AST node.
 * @param[in] sourceCode Document source text.
 * @return Extracted string_view, or empty view if node is null or out of bounds.
 */
[[nodiscard]] inline std::string_view GetNodeTextView(TSNode node, std::string_view sourceCode) noexcept
{
    return NodeText(node, sourceCode).view;
}

/**
 * @brief Checks if a string view begins and ends with matching quotes.
 * @param[in] raw Raw string literal view.
 * @return True if enclosed in single, double, or triple quotes.
 */
[[nodiscard]] constexpr bool IsQuotedString(std::string_view raw) noexcept
{
    return (raw.starts_with("\"\"\"") && raw.size() >= 6 && raw.ends_with("\"\"\"")) ||
           (raw.size() >= 2 &&
            ((raw.front() == '"' && raw.back() == '"') || (raw.front() == '\'' && raw.back() == '\'')));
}

/**
 * @brief Safely removes enclosing quotes from a single-line or multiline string literal view.
 * @param[in] raw Raw string literal view.
 * @return Unquoted string view without enclosing quotes or delimiters.
 */
[[nodiscard]] constexpr std::string_view UnquoteStringView(std::string_view raw) noexcept
{
    if (raw.starts_with("\"\"\""))
    {
        if (raw.size() >= 6 && raw.ends_with("\"\"\""))
        {
            return raw.substr(3, raw.size() - 6);
        }
        return raw.substr(3);
    }
    if (raw.size() >= 2 && ((raw.front() == '"' && raw.back() == '"') || (raw.front() == '\'' && raw.back() == '\'')))
    {
        return raw.substr(1, raw.size() - 2);
    }
    if (!raw.empty() && (raw.front() == '"' || raw.front() == '\''))
    {
        return raw.substr(1);
    }
    return raw;
}

/**
 * @brief Performs a flat, non-recursive preorder traversal over all AST descendants.
 * @tparam Callback Callable with signature void(TSNode).
 * @param[in] root Root AST node to traverse.
 * @param[in] callback Visitor callback invoked for each node.
 */
template <typename Callback> void ForEachDescendantNode(TSNode root, Callback&& callback)
{
    if (ts_node_is_null(root))
    {
        return;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool visiting = true;

    while (visiting)
    {
        const TSNode current = ts_tree_cursor_current_node(&cursor);
        callback(current);

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }

        bool backtracked = false;
        while (ts_tree_cursor_goto_parent(&cursor))
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                backtracked = true;
                break;
            }
        }
        if (!backtracked)
        {
            visiting = false;
        }
    }

    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Iterates over direct child nodes of an AST node using a flat TSTreeCursor.
 * @tparam Callback Callable with signature void(TSNode).
 * @param[in] parent Parent AST node.
 * @param[in] callback Visitor callback invoked for each direct child node.
 */
template <typename Callback> void ForEachChildNode(TSNode parent, Callback&& callback)
{
    if (ts_node_is_null(parent))
    {
        return;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(parent);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            callback(ts_tree_cursor_current_node(&cursor));
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Iterates over direct named child nodes of an AST node using a flat TSTreeCursor.
 * @tparam Callback Callable with signature void(TSNode).
 * @param[in] parent Parent AST node.
 * @param[in] callback Visitor callback invoked for each direct named child node.
 */
template <typename Callback> void ForEachNamedChildNode(TSNode parent, Callback&& callback)
{
    if (ts_node_is_null(parent))
    {
        return;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(parent);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode node = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(node))
            {
                callback(node);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Iterates over direct child nodes until predicate returns false.
 * @tparam Predicate Callable with signature bool(TSNode) returning false to stop traversal.
 * @param[in] parent Parent AST node.
 * @param[in] predicate Visitor invoked for each direct child node.
 * @return True if iteration completed without early interruption; false if stopped early.
 */
template <typename Predicate> bool ForEachChildNodeUntil(TSNode parent, Predicate&& predicate)
{
    if (ts_node_is_null(parent))
    {
        return true;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(parent);
    bool completed = true;
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode node = ts_tree_cursor_current_node(&cursor);
            if (!predicate(node))
            {
                completed = false;
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return completed;
}

/**
 * @brief Iterates over direct named child nodes until predicate returns false.
 * @tparam Predicate Callable with signature bool(TSNode) returning false to stop traversal.
 * @param[in] parent Parent AST node.
 * @param[in] predicate Visitor invoked for each direct named child node.
 * @return True if iteration completed without early interruption; false if stopped early.
 */
template <typename Predicate> bool ForEachNamedChildNodeUntil(TSNode parent, Predicate&& predicate)
{
    if (ts_node_is_null(parent))
    {
        return true;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(parent);
    bool completed = true;
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode node = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(node))
            {
                if (!predicate(node))
                {
                    completed = false;
                    break;
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return completed;
}

/**
 * @brief Finds the first direct child node matching a specific Tree-Sitter grammar symbol.
 * @param[in] parent Parent AST node.
 * @param[in] sym Target grammar symbol.
 * @return Matching child TSNode or null TSNode if not found.
 */
[[nodiscard]] inline TSNode FindFirstChildOfSymbol(TSNode parent, TSSymbol sym) noexcept
{
    TSNode found{};
    ForEachChildNodeUntil(parent,
                          [&](TSNode child)
                          {
                              if (!ts_node_is_null(child) && ts_node_symbol(child) == sym)
                              {
                                  found = child;
                                  return false;
                              }
                              return true;
                          });
    return found;
}

/**
 * @brief Finds the first direct child node matching a specific Tree-Sitter node type.
 * @param[in] parent Parent AST node.
 * @param[in] typeName Target node type string.
 * @return Matching child TSNode or null TSNode if not found.
 */
inline TSNode FindFirstChildOfType(TSNode parent, std::string_view typeName)
{
    TSNode found{};
    ForEachChildNodeUntil(parent,
                          [&](TSNode child)
                          {
                              if (!ts_node_is_null(child) && std::string_view(ts_node_type(child)) == typeName)
                              {
                                  found = child;
                                  return false;
                              }
                              return true;
                          });
    return found;
}

/**
 * @brief Finds the nearest ancestor node matching a specific Tree-Sitter grammar symbol.
 * @param[in] node Starting child AST node.
 * @param[in] targetSym Target grammar symbol to search for.
 * @param[in] maxDepth Maximum upward climb depth (bounded by k_maxAstClimbDepth).
 * @return Matching ancestor TSNode or null TSNode if not found.
 */
[[nodiscard]] inline TSNode FindAncestorWithSymbol(TSNode node, TSSymbol targetSym,
                                                   uint32_t maxDepth = constants::k_maxAstClimbDepth) noexcept
{
    TSNode curr = node;
    uint32_t depth = 0;
    while (!ts_node_is_null(curr) && depth < maxDepth)
    {
        if (ts_node_symbol(curr) == targetSym)
        {
            return curr;
        }
        curr = GetParent(curr);
        depth++;
    }
    return TSNode{};
}

/**
 * @brief Finds the nearest ancestor node matching any of the specified Tree-Sitter grammar symbols.
 * @param[in] node Starting child AST node.
 * @param[in] targetSyms Span of target grammar symbols.
 * @param[in] maxDepth Maximum upward climb depth (bounded by k_maxAstClimbDepth).
 * @return Matching ancestor TSNode or null TSNode if not found.
 */
[[nodiscard]] inline TSNode FindAncestorWithAnySymbol(TSNode node, std::span<const TSSymbol> targetSyms,
                                                      uint32_t maxDepth = constants::k_maxAstClimbDepth) noexcept
{
    TSNode curr = node;
    uint32_t depth = 0;
    while (!ts_node_is_null(curr) && depth < maxDepth)
    {
        const TSSymbol sym = ts_node_symbol(curr);
        for (const TSSymbol target : targetSyms)
        {
            if (sym == target)
            {
                return curr;
            }
        }
        curr = GetParent(curr);
        depth++;
    }
    return TSNode{};
}

/**
 * @brief Finds the enclosing node of a given symbol type at the specified point range.
 * @param[in] root Root AST node.
 * @param[in] point Document point (row, column).
 * @param[in] targetSym Target grammar symbol to locate.
 * @return Matching ancestor TSNode or null TSNode if not found.
 */
[[nodiscard]] inline TSNode FindEnclosingNodeAtPoint(TSNode root, TSPoint point, TSSymbol targetSym) noexcept
{
    if (ts_node_is_null(root))
    {
        return TSNode{};
    }
    TSNode descendant = ts_node_descendant_for_point_range(root, point, point);
    return FindAncestorWithSymbol(descendant, targetSym);
}

} // namespace angel_lsp::parser
