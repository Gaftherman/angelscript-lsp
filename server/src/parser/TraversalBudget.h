#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{

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
 * @note Carries the visit count and ceiling so harnesses can report how far over budget a pass ran.
 */
class BudgetExceededException : public std::runtime_error
{
  public:
    /**
     * @brief Builds the exception with a message naming both counters.
     * @param[in] visits Visits recorded when the ceiling was crossed.
     * @param[in] maxAllowed Ceiling the traversal was allowed to reach.
     */
    explicit BudgetExceededException(size_t visits, size_t maxAllowed)
        : std::runtime_error("AST traversal budget exceeded: " + std::to_string(visits) +
                             " visits exceeds ceiling of " + std::to_string(maxAllowed)),
          m_visits(visits), m_maxAllowed(maxAllowed)
    {
    }

    /**
     * @brief Returns the visit count recorded when the ceiling was crossed.
     * @return Number of visits, always greater than GetMaxAllowed().
     */
    [[nodiscard]] size_t GetVisits() const noexcept
    {
        return m_visits;
    }

    /**
     * @brief Returns the ceiling the traversal exceeded.
     * @return Maximum number of visits that was allowed.
     */
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
 * @note A ceiling of zero (default construction) disables enforcement.
 */
class TraversalBudget
{
  public:
    TraversalBudget() = default;

    /**
     * @brief Sets the ceiling to a multiple of a known named-node count.
     * @param[in] namedNodeCount Named nodes in the tree being traversed.
     * @param[in] multiplier Visits allowed per named node.
     */
    explicit TraversalBudget(size_t namedNodeCount, double multiplier = 3.0)
        : m_namedNodeCount(namedNodeCount),
          m_maxAllowed(static_cast<size_t>(static_cast<double>(namedNodeCount) * multiplier))
    {
    }

    /**
     * @brief Sets the ceiling from the named nodes counted under root.
     * @param[in] root Root of the tree being traversed.
     * @param[in] multiplier Visits allowed per named node.
     */
    explicit TraversalBudget(TSNode root, double multiplier = 3.0) : TraversalBudget(CountNamedNodes(root), multiplier)
    {
    }

    /**
     * @brief Records visits and enforces the ceiling.
     * @param[in] count Number of visits to add.
     * @throws BudgetExceededException When the total exceeds a non-zero ceiling.
     */
    void RecordVisit(size_t count = 1)
    {
        m_currentVisits += count;
        if (m_maxAllowed > 0 && m_currentVisits > m_maxAllowed)
        {
            throw BudgetExceededException(m_currentVisits, m_maxAllowed);
        }
    }

    /**
     * @brief Returns the visits recorded so far.
     * @return Total visits.
     */
    [[nodiscard]] size_t GetCurrentVisits() const noexcept
    {
        return m_currentVisits;
    }

    /**
     * @brief Returns the visit ceiling.
     * @return Ceiling, or zero when enforcement is disabled.
     */
    [[nodiscard]] size_t GetMaxAllowed() const noexcept
    {
        return m_maxAllowed;
    }

    /**
     * @brief Returns the named-node count the ceiling was derived from.
     * @return Named-node count.
     */
    [[nodiscard]] size_t GetNamedNodeCount() const noexcept
    {
        return m_namedNodeCount;
    }

    /**
     * @brief Reports whether the recorded visits passed a non-zero ceiling.
     * @return True once the budget is exhausted.
     */
    [[nodiscard]] bool IsExceeded() const noexcept
    {
        return m_maxAllowed > 0 && m_currentVisits > m_maxAllowed;
    }

  private:
    size_t m_namedNodeCount = 0;
    size_t m_maxAllowed = 0;
    size_t m_currentVisits = 0;
};

} // namespace angel_lsp::parser
