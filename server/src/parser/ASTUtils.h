#pragma once

#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "utils/Constants.h"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{
[[nodiscard]] inline std::string_view GetNodeType(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const char* t = ts_node_type(node);
    return t ? std::string_view(t) : std::string_view{};
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
 * @brief Finds the first direct child node matching the given grammar symbol.
 * @param[in] parent Parent AST node.
 * @param[in] targetSym Grammar symbol to match.
 * @return First matching child TSNode, or a null node if none found.
 */
[[nodiscard]] inline TSNode FindChildWithSymbol(TSNode parent, TSSymbol targetSym) noexcept
{
    if (ts_node_is_null(parent))
    {
        return TSNode{};
    }
    TSTreeCursor cursor = ts_tree_cursor_new(parent);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(child) == targetSym)
            {
                ts_tree_cursor_delete(&cursor);
                return child;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return TSNode{};
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
 * @return Matching ancestor TSNode or null TSNode if not found.
 */
[[nodiscard]] inline TSNode FindAncestorWithSymbol(TSNode node, TSSymbol targetSym) noexcept
{
    TSNode curr = node;
    while (!ts_node_is_null(curr))
    {
        if (ts_node_symbol(curr) == targetSym)
        {
            return curr;
        }
        curr = GetParent(curr);
    }
    return TSNode{};
}

/**
 * @brief Finds the nearest ancestor node matching any of the specified Tree-Sitter grammar symbols.
 * @param[in] node Starting child AST node.
 * @param[in] targetSyms Span of target grammar symbols.
 * @return Matching ancestor TSNode or null TSNode if not found.
 */
[[nodiscard]] inline TSNode FindAncestorWithAnySymbol(TSNode node, std::span<const TSSymbol> targetSyms) noexcept
{
    TSNode curr = node;
    while (!ts_node_is_null(curr))
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
    }
    return TSNode{};
}

/**
 * @brief Finds the direct child node of an ancestor matching ancestorSym.
 * @param[in] node Starting child AST node.
 * @param[in] ancestorSym Target ancestor grammar symbol.
 * @return Direct child of matching ancestor containing node, or null TSNode.
 */
[[nodiscard]] inline TSNode FindDirectChildOfAncestor(TSNode node, TSSymbol ancestorSym) noexcept
{
    TSNode curr = node;
    TSNode parent = GetParent(curr);
    while (!ts_node_is_null(parent))
    {
        if (ts_node_symbol(parent) == ancestorSym)
        {
            return curr;
        }
        curr = parent;
        parent = GetParent(curr);
    }
    return TSNode{};
}

/**
 * @brief Finds the outermost ancestor matching targetSym by climbing to root.
 * @param[in] node Starting child AST node.
 * @param[in] targetSym Target grammar symbol to search for.
 * @return Outermost matching ancestor TSNode or null TSNode if none match.
 */
[[nodiscard]] inline TSNode FindOutermostAncestorWithSymbol(TSNode node, TSSymbol targetSym) noexcept
{
    TSNode result{};
    TSNode curr = node;
    while (!ts_node_is_null(curr))
    {
        if (ts_node_symbol(curr) == targetSym)
        {
            result = curr;
        }
        curr = GetParent(curr);
    }
    return result;
}

/**
 * @brief Traverses ancestors from node up to root, invoking callback(curr).
 *        Stops traversal early if callback returns false.
 * @tparam Callback Callable with signature bool(TSNode) or void(TSNode).
 * @param[in] node Starting child AST node.
 * @param[in] callback Visitor function.
 */
template <typename Callback> inline void ForEachAncestor(TSNode node, Callback&& callback) noexcept
{
    TSNode curr = node;
    while (!ts_node_is_null(curr))
    {
        if constexpr (std::is_invocable_r_v<bool, Callback, TSNode>)
        {
            if (!callback(curr))
            {
                break;
            }
        }
        else
        {
            callback(curr);
        }
        curr = GetParent(curr);
    }
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

/**
 * @brief Trims leading and trailing ASCII whitespace from a string view.
 * @param text The input string view.
 * @return The trimmed string view.
 */
[[nodiscard]] inline std::string_view Trim(std::string_view text) noexcept
{
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
    {
        text.remove_suffix(1);
    }
    return text;
}

/**
 * @brief Represents the global scope qualification of an AST node.
 */
struct GlobalScopeQualifier
{
    bool isGlobal = false;       ///< True if prefixed with leading "::"
    bool isDirectRoot = false;   ///< True if direct root-global (::Ident with no namespace path)
    std::string_view rootPath{}; ///< Path after leading "::", e.g. "Deploy" or "NS::Deploy"
};

/**
 * @brief Analyzes whether an AST node is globally qualified (starts with leading "::").
 * @param[in] node AST node (identifier, scoped_identifier, etc.).
 * @param[in] sourceCode Source text.
 * @return GlobalScopeQualifier structure.
 */
[[nodiscard]] inline GlobalScopeQualifier GetGlobalScopeQualifier(TSNode node, std::string_view sourceCode) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode target = node;
    const TSSymbol nSym = ts_node_symbol(node);
    if (nSym != gs.symScopedIdentifier)
    {
        TSNode parent = parser::GetParent(node);
        if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symScopedIdentifier)
        {
            target = parent;
        }
        else
        {
            return {};
        }
    }

    if (ts_node_child_count(target) == 0)
    {
        return {};
    }

    TSNode firstChild = ts_node_child(target, 0);
    if (ts_node_symbol(firstChild) != gs.tokColonColon)
    {
        return {};
    }

    GlobalScopeQualifier res;
    res.isGlobal = true;

    const uint32_t start = ts_node_start_byte(target);
    const uint32_t end = ts_node_end_byte(target);
    if (start < end && end <= sourceCode.size() && (start + 2) <= end)
    {
        res.rootPath = sourceCode.substr(start + 2, end - (start + 2));
    }

    const uint32_t namedCount = ts_node_named_child_count(target);
    res.isDirectRoot = (namedCount == 1);
    return res;
}

/**
 * @brief Checks if an expression node is a handle assignment target (e.g. '@target = ...').
 * @param[in] target Target AST node to test.
 * @return True if target is a unary expression whose operator is '@'.
 */
[[nodiscard]] inline bool IsHandleAssignmentTarget(TSNode target) noexcept
{
    if (ts_node_is_null(target))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(target) != gs.symUnaryExpression)
    {
        return false;
    }
    const TSNode prefix = parser::GetChildByField(target, gs.fieldOperator);
    return !ts_node_is_null(prefix) && ts_node_symbol(prefix) == gs.tokAt;
}

/**
 * @brief Finds the initializer value node for a variable declarator.
 * @param[in] declaratorNode Variable declarator syntax node.
 * @param[in] sourceCode Document source text.
 * @return Value expression node, or a null node if none found.
 */
[[nodiscard]] inline TSNode FindDeclaratorValueNode(TSNode declaratorNode, std::string_view sourceCode) noexcept
{
    if (ts_node_is_null(declaratorNode))
    {
        return {};
    }
    TSNode valueNode = parser::GetChildByField(declaratorNode, parser::GetGrammarSymbols().fieldValue);
    if (!ts_node_is_null(valueNode))
    {
        return valueNode;
    }

    const uint32_t childCount = ts_node_child_count(declaratorNode);
    bool foundEq = false;
    for (uint32_t c = 0; c < childCount; ++c)
    {
        TSNode ch = ts_node_child(declaratorNode, c);
        if (foundEq)
        {
            return ch;
        }
        if (parser::NodeText(ch, sourceCode) == "=")
        {
            foundEq = true;
        }
    }
    return {};
}

/**
 * @brief Summary of active braces and unmatched positions across a source file.
 */
struct BraceBalance
{
    int openBraces = 0;
    int closeBraces = 0;
    int firstUnmatchedCloseLine = -1;
    uint32_t firstUnmatchedCloseByte = UINT32_MAX;

    [[nodiscard]] bool HasMissingOpenBrace() const noexcept
    {
        return openBraces < closeBraces;
    }
};

inline void SkipComment(std::string_view sourceCode, size_t& i, int& currentLine) noexcept
{
    const size_t len = sourceCode.size();
    if (sourceCode[i + 1] == '/')
    {
        i += 2;
        while (i < len && sourceCode[i] != '\n')
            ++i;
    }
    else if (sourceCode[i + 1] == '*')
    {
        i += 2;
        while (i + 1 < len && !(sourceCode[i] == '*' && sourceCode[i + 1] == '/'))
        {
            if (sourceCode[i] == '\n')
                ++currentLine;
            ++i;
        }
        i = (i + 1 < len) ? (i + 2) : len;
    }
}

/**
 * @brief Advances index past a triple-quoted heredoc literal (""" ... """).
 * @param[in] sourceCode Document source text.
 * @param[in,out] i Character index in source text.
 * @param[in,out] currentLine Current line count.
 */
inline void SkipHeredocLiteral(std::string_view sourceCode, size_t& i, int& currentLine) noexcept
{
    i += 3;
    const size_t len = sourceCode.size();
    while (i < len)
    {
        if (sourceCode[i] == '\n')
        {
            ++currentLine;
            ++i;
        }
        else if (sourceCode.substr(i).starts_with("\"\"\""))
        {
            i += 3;
            return;
        }
        else
        {
            ++i;
        }
    }
}

/**
 * @brief Advances index past a string or character literal, stopping at newlines for unclosed literals.
 * @param[in] sourceCode Document source text.
 * @param[in,out] i Character index in source text.
 * @param[in,out] currentLine Current line count.
 */
inline void SkipLiteral(std::string_view sourceCode, size_t& i, int& currentLine) noexcept
{
    if (sourceCode.substr(i).starts_with("\"\"\""))
    {
        SkipHeredocLiteral(sourceCode, i, currentLine);
        return;
    }
    const size_t len = sourceCode.size();
    const char quote = sourceCode[i++];
    while (i < len && sourceCode[i] != quote)
    {
        if (sourceCode[i] == '\\' && i + 1 < len)
        {
            if (sourceCode[i + 1] == '\n')
            {
                ++currentLine;
            }
            i += 2;
            continue;
        }
        if (sourceCode[i] == '\n' || sourceCode[i] == '\r')
        {
            break;
        }
        ++i;
    }
    if (i < len && sourceCode[i] == quote)
    {
        ++i;
    }
}

/**
 * @brief Fast single-pass calculation of brace balance outside comments and literals.
 * @param[in] sourceCode Document source text.
 * @return Computed BraceBalance metrics.
 */
[[nodiscard]] inline BraceBalance ComputeBraceBalance(std::string_view sourceCode) noexcept
{
    BraceBalance balance;
    int currentDepth = 0;
    int currentLine = 0;
    const size_t len = sourceCode.size();
    size_t i = 0;

    while (i < len)
    {
        const char c = sourceCode[i];
        if (c == '\n')
        {
            ++currentLine;
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < len && (sourceCode[i + 1] == '/' || sourceCode[i + 1] == '*'))
        {
            SkipComment(sourceCode, i, currentLine);
            continue;
        }
        if (c == '"' || c == '\'')
        {
            SkipLiteral(sourceCode, i, currentLine);
            continue;
        }
        if (c == '{')
        {
            ++balance.openBraces;
            ++currentDepth;
        }
        else if (c == '}')
        {
            ++balance.closeBraces;
            --currentDepth;
            if (currentDepth < 0 && balance.firstUnmatchedCloseLine == -1)
            {
                balance.firstUnmatchedCloseLine = currentLine;
                balance.firstUnmatchedCloseByte = static_cast<uint32_t>(i);
            }
        }
        ++i;
    }
    return balance;
}

/**
 * @brief Checks if an identifier or scoped identifier has leading global scope qualifier ("::").
 * @param[in] node AST node.
 * @param[in] sourceCode Document source text.
 * @return True if qualified strictly by leading global "::".
 */
[[nodiscard]] inline bool IsGlobalScopedNode(TSNode node, std::string_view sourceCode) noexcept
{
    return GetGlobalScopeQualifier(node, sourceCode).isGlobal;
}

/**
 * @brief Counts the total number of named AST nodes in the subtree rooted at root.
 * @param[in] root Root node of the AST subtree.
 * @return Count of named nodes in the subtree.
 */
[[nodiscard]] inline size_t CountNamedNodes(TSNode root) noexcept
{
    if (ts_node_is_null(root))
    {
        return 0;
    }
    size_t count = 0;
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool hasChild = ts_tree_cursor_goto_first_child(&cursor);
    if (!hasChild)
    {
        ts_tree_cursor_delete(&cursor);
        return ts_node_is_named(root) ? 1 : 0;
    }
    if (ts_node_is_named(root))
    {
        ++count;
    }
    while (true)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_named(node))
        {
            ++count;
        }
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }
        bool found = false;
        while (ts_tree_cursor_goto_parent(&cursor))
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            break;
        }
    }
    ts_tree_cursor_delete(&cursor);
    return count;
}

} // namespace angel_lsp::parser

namespace angel_lsp::analysis
{
using parser::ForEachChildNode;
using parser::ForEachNamedChildNode;
using parser::ForEachNamedChildNodeUntil;
using parser::GetNodeText;
using parser::GetParent;
using parser::GetRootNode;
using parser::NodeText;
using parser::NodeTextResult;
using parser::NodeTextString;
using parser::UnquoteStringView;

using parser::BraceBalance;
using parser::ComputeBraceBalance;
using parser::FindDeclaratorValueNode;
using parser::GetGlobalScopeQualifier;
using parser::GlobalScopeQualifier;
using parser::IsGlobalScopedNode;
using parser::IsHandleAssignmentTarget;
using parser::SkipComment;
using parser::SkipHeredocLiteral;
using parser::SkipLiteral;
using parser::Trim;

using parser::CountNamedNodes;

/**
 * @brief Exception thrown when an AST traversal or checker exceeds its node visit budget.
 */
class BudgetExceededException : public std::runtime_error
{
  public:
    explicit BudgetExceededException(size_t visits, size_t maxAllowed)
        : std::runtime_error("AST traversal budget exceeded: " + std::to_string(visits) +
                             " visits exceeds ceiling of " + std::to_string(maxAllowed)),
          m_visits(visits), m_maxAllowed(maxAllowed)
    {
    }

    explicit BudgetExceededException(const std::string& message)
        : std::runtime_error(message), m_visits(0), m_maxAllowed(0)
    {
    }

    [[nodiscard]] size_t GetVisits() const noexcept
    {
        return m_visits;
    }

    [[nodiscard]] size_t GetMaxAllowed() const noexcept
    {
        return m_maxAllowed;
    }

  private:
    size_t m_visits = 0;
    size_t m_maxAllowed = 0;
};

/**
 * @brief Enforces an iteration ceiling on AST node traversals relative to named nodes.
 */
class TraversalBudget
{
  public:
    TraversalBudget() = default;

    explicit TraversalBudget(size_t namedNodeCount, double multiplier = 3.0)
        : m_namedNodeCount(namedNodeCount),
          m_maxAllowed(static_cast<size_t>(static_cast<double>(namedNodeCount) * multiplier)), m_currentVisits(0)
    {
    }

    explicit TraversalBudget(TSNode root, double multiplier = 3.0) : TraversalBudget(CountNamedNodes(root), multiplier)
    {
    }

    void RecordVisit(size_t count = 1)
    {
        m_currentVisits += count;
        if (m_maxAllowed > 0 && m_currentVisits > m_maxAllowed)
        {
            throw BudgetExceededException(m_currentVisits, m_maxAllowed);
        }
    }

    void RecordVisit([[maybe_unused]] TSNode node)
    {
        RecordVisit(1);
    }

    [[nodiscard]] size_t GetCurrentVisits() const noexcept
    {
        return m_currentVisits;
    }

    [[nodiscard]] size_t GetMaxAllowed() const noexcept
    {
        return m_maxAllowed;
    }

    [[nodiscard]] size_t GetNamedNodeCount() const noexcept
    {
        return m_namedNodeCount;
    }

    [[nodiscard]] bool IsExceeded() const noexcept
    {
        return m_maxAllowed > 0 && m_currentVisits > m_maxAllowed;
    }

    void Reset() noexcept
    {
        m_currentVisits = 0;
    }

    void SetRoot(TSNode root, double multiplier = 3.0)
    {
        m_namedNodeCount = CountNamedNodes(root);
        m_maxAllowed = static_cast<size_t>(static_cast<double>(m_namedNodeCount) * multiplier);
        m_currentVisits = 0;
    }

  private:
    size_t m_namedNodeCount = 0;
    size_t m_maxAllowed = 0;
    size_t m_currentVisits = 0;
};

} // namespace angel_lsp::analysis
