#pragma once

#include "parser/ASTUtils.h"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
using parser::ForEachChildNode;
using parser::ForEachDescendantNode;
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
