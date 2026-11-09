#pragma once

#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "utils/Constants.h"

#include <cstdint>
#include <span>
#include <stdexcept>
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
 * @brief Strips outer enclosing parentheses from an expression node.
 * @param[in] node AST node to unwrap.
 * @return Innermost non-parenthesized expression node, or null node if input is null.
 */
[[nodiscard]] inline TSNode UnwrapParentheses(TSNode node) noexcept
{
    const auto& gs = GetGrammarSymbols();
    while (!ts_node_is_null(node) && ts_node_symbol(node) == gs.symParenthesizedExpression &&
           ts_node_named_child_count(node) > 0)
    {
        node = ts_node_named_child(node, 0);
    }
    return node;
}

/**
 * @brief Extracts the raw source text corresponding to an AST node.
 * @param[in] node The TSNode whose slice to extract.
 * @param[in] sourceCode The full document source code.
 * @return View into sourceCode spanning the node's byte range; empty when the node is null or out of bounds.
 * @warning The view borrows sourceCode: copy it into a std::string before storing it beyond the buffer's lifetime.
 */
[[nodiscard]] inline std::string_view NodeText(TSNode node, std::string_view sourceCode) noexcept
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
    return sourceCode.substr(start, end - start);
}

/**
 * @brief Returns the grammar spelling of an anonymous operator token.
 * @param[in] opNode Operator token node, typically the `operator` field of an expression.
 * @return Token literal taken from the grammar (for example "+=" or "!is"); empty for null or named nodes.
 * @note The view points into static language tables, so it never borrows the document text.
 */
[[nodiscard]] inline std::string_view OperatorSpelling(TSNode opNode) noexcept
{
    if (ts_node_is_null(opNode) || ts_node_is_named(opNode))
    {
        return {};
    }
    return ts_node_type(opNode);
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
 * @tparam Predicate Callable with signature bool(TSNode) or bool(TSNode, TSFieldId), returning false to stop traversal.
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
            const bool keepGoing = [&]
            {
                if constexpr (requires { predicate(node, TSFieldId{}); })
                {
                    return predicate(node, ts_tree_cursor_current_field_id(&cursor));
                }
                else
                {
                    return predicate(node);
                }
            }();
            if (!keepGoing)
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
 * @brief Global scope qualification of an AST node.
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
 * @brief Checks if a tree-sitter node symbol corresponds to an entity declaration.
 * @param[in] sym Tree-sitter grammar symbol.
 * @return True if symbol is a recognized declaration node.
 */
[[nodiscard]] inline bool IsDeclarationSymbol(TSSymbol sym) noexcept
{
    const auto& gs = GetGrammarSymbols();
    return sym == gs.symFuncDeclaration || sym == gs.symVariableDeclarator || sym == gs.symParameter ||
           sym == gs.symClassDeclaration || sym == gs.symInterfaceDeclaration || sym == gs.symInterfaceMethod ||
           sym == gs.symEnumDeclaration || sym == gs.symEnumMember || sym == gs.symFuncdefDeclaration ||
           sym == gs.symTypedefDeclaration || sym == gs.symNamespaceDeclaration || sym == gs.symForeachVariable ||
           sym == gs.symVirtualProperty || sym == gs.symImportDeclaration || sym == gs.symMixinDeclaration;
}

} // namespace angel_lsp::parser
