#pragma once

#include <cstdint>
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

} // namespace angel_lsp::parser
