#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Hard ceiling on how deep any recursive walk over a syntax tree may go.
 *
 * Tree-sitter imposes no depth limit and happily builds a tree as deeply nested as the source
 * is, so a document consisting of a few thousand `(` characters - well under any reasonable
 * size limit - produced a tree deep enough to overflow the stack in every checker that walks it
 * recursively. There are a dozen such walks per analysis and each one is a separate crash.
 *
 * Source nested past this is pathological by definition, and abandoning a subtree costs only a
 * diagnostic - the safe direction for this analyzer, whose stated policy is that a missed error
 * costs nothing and a false one costs the user's trust in every other diagnostic on screen.
 * SymbolCollector::ReportParseErrors already had its own cap for exactly this reason; this is
 * that idea applied everywhere it was missing.
 *
 * The number is low on purpose, and was measured rather than guessed: 512, 256 and 128 all
 * still overflowed, and 64 is the first value that survives. Two things make the usable budget
 * far smaller than the raw stack size suggests:
 *
 *  - The frames are large. The expression resolvers hold several std::string locals apiece.
 *  - The caps compose. A guarded checker recursing 64 deep calls a resolver that then begins
 *    its own 64-deep budget, so the real worst case is a multiple of this number.
 *
 * Debug frames are larger than Release ones and this has to hold in the build developers
 * actually run, so the limit is set for the worse case.
 */
inline constexpr int k_maxAstDepth = 64;

/**
 * @brief Returns the AST node type name as a zero-allocation string view.
 * @param node The TSNode to inspect.
 * @return String view containing the node type name.
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
    constexpr NodeTextResult(std::string_view v) noexcept : view(v) {}

    constexpr operator std::string_view() const noexcept { return view; }
    operator std::string() const { return std::string(view); }

    [[nodiscard]] constexpr bool empty() const noexcept { return view.empty(); }
    [[nodiscard]] constexpr size_t size() const noexcept { return view.size(); }
    [[nodiscard]] constexpr size_t length() const noexcept { return view.length(); }
    [[nodiscard]] constexpr const char* data() const noexcept { return view.data(); }
    [[nodiscard]] constexpr char front() const { return view.front(); }
    [[nodiscard]] constexpr char back() const { return view.back(); }
    [[nodiscard]] constexpr char operator[](size_t i) const { return view[i]; }

    static constexpr size_t npos = std::string_view::npos;
    [[nodiscard]] size_t find(std::string_view s, size_t pos = 0) const noexcept { return view.find(s, pos); }
    [[nodiscard]] size_t find(char c, size_t pos = 0) const noexcept { return view.find(c, pos); }
    [[nodiscard]] bool starts_with(std::string_view s) const noexcept { return view.starts_with(s); }
    [[nodiscard]] bool ends_with(std::string_view s) const noexcept { return view.ends_with(s); }
    [[nodiscard]] NodeTextResult substr(size_t pos = 0, size_t count = std::string_view::npos) const noexcept
    {
        return NodeTextResult(view.substr(pos, count));
    }

    friend bool operator==(const NodeTextResult& lhs, std::string_view rhs) noexcept { return lhs.view == rhs; }
    friend bool operator==(std::string_view lhs, const NodeTextResult& rhs) noexcept { return lhs == rhs.view; }
    friend bool operator==(const NodeTextResult& lhs, const NodeTextResult& rhs) noexcept { return lhs.view == rhs.view; }
    friend bool operator!=(const NodeTextResult& lhs, std::string_view rhs) noexcept { return lhs.view != rhs; }
    friend bool operator!=(std::string_view lhs, const NodeTextResult& rhs) noexcept { return lhs != rhs.view; }
    friend bool operator!=(const NodeTextResult& lhs, const NodeTextResult& rhs) noexcept { return lhs.view != rhs.view; }

    friend std::string operator+(const std::string& lhs, const NodeTextResult& rhs) { return lhs + std::string(rhs.view); }
    friend std::string operator+(const NodeTextResult& lhs, const std::string& rhs) { return std::string(lhs.view) + rhs; }
};

/**
 * @brief Extracts the raw source text corresponding to an AST node.
 * @param node The TSNode whose slice to extract.
 * @param sourceCode The full document source code.
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
 * @param node The TSNode whose slice to extract.
 * @param sourceCode The full document source code.
 * @return std::string spanning the node's byte range.
 */
[[nodiscard]] inline std::string NodeTextString(TSNode node, std::string_view sourceCode)
{
    return std::string(NodeText(node, sourceCode));
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
    bool visiting = true;

    while (visiting)
    {
        const TSNode current = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_named(current))
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
    return count;
}

/**
 * @brief Exception thrown when an AST traversal or checker exceeds its node visit budget.
 */
class BudgetExceededException : public std::runtime_error
{
  public:
    /**
     * @brief Constructs a BudgetExceededException with node visit metrics.
     * @param[in] visits Total node visits recorded when budget was exceeded.
     * @param[in] maxAllowed Maximum allowed node visits under the budget.
     */
    explicit BudgetExceededException(size_t visits, size_t maxAllowed)
        : std::runtime_error("AST traversal budget exceeded: " + std::to_string(visits) +
                             " visits exceeds ceiling of " + std::to_string(maxAllowed)),
          m_visits(visits), m_maxAllowed(maxAllowed)
    {
    }

    /**
     * @brief Constructs a BudgetExceededException with an explicit message.
     * @param[in] message Custom diagnostic explanation.
     */
    explicit BudgetExceededException(const std::string& message)
        : std::runtime_error(message), m_visits(0), m_maxAllowed(0)
    {
    }

    /**
     * @brief Returns total visits attempted.
     * @return Number of node visits.
     */
    [[nodiscard]] size_t GetVisits() const noexcept
    {
        return m_visits;
    }

    /**
     * @brief Returns maximum allowed visits under the budget.
     * @return Budget ceiling.
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
 * @brief Enforces an $O(N)$ iteration ceiling on AST node traversals relative to named nodes.
 *
 * Exceeding 3 * M_nodes throws BudgetExceededException to ensure checkers maintain linear complexity.
 */
class TraversalBudget
{
  public:
    /**
     * @brief Default constructor creating an unconstrained budget.
     */
    TraversalBudget() = default;

    /**
     * @brief Constructs a budget tied to named node count with a multiplier.
     * @param[in] namedNodeCount Total named nodes in the syntax tree ($M_{nodes}$).
     * @param[in] multiplier Visit multiplier relative to named nodes (defaults to 3.0).
     */
    explicit TraversalBudget(size_t namedNodeCount, double multiplier = 3.0)
        : m_namedNodeCount(namedNodeCount),
          m_maxAllowed(static_cast<size_t>(static_cast<double>(namedNodeCount) * multiplier)), m_currentVisits(0)
    {
    }

    /**
     * @brief Constructs a budget directly from an AST root node.
     * @param[in] root Root node of the AST.
     * @param[in] multiplier Visit multiplier relative to named nodes (defaults to 3.0).
     */
    explicit TraversalBudget(TSNode root, double multiplier = 3.0) : TraversalBudget(CountNamedNodes(root), multiplier)
    {
    }

    /**
     * @brief Records a batch of AST node visits and enforces the budget ceiling.
     * @param[in] count Number of visits to record.
     * @throws BudgetExceededException if current visits exceed maxAllowed.
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
     * @brief Records a single AST node visit and enforces the budget ceiling.
     * @param[in] node AST node visited.
     * @note Silences unused node parameter while fulfilling the AST visit contract.
     * @throws BudgetExceededException if current visits exceed maxAllowed.
     */
    void RecordVisit([[maybe_unused]] TSNode node)
    {
        RecordVisit(1);
    }

    /**
     * @brief Returns current accumulated node visits.
     * @return Accumulated visit count.
     */
    [[nodiscard]] size_t GetCurrentVisits() const noexcept
    {
        return m_currentVisits;
    }

    /**
     * @brief Returns the maximum visits permitted under this budget.
     * @return Maximum allowed visits.
     */
    [[nodiscard]] size_t GetMaxAllowed() const noexcept
    {
        return m_maxAllowed;
    }

    /**
     * @brief Returns the named node count ($M_{nodes}$) used to configure the budget.
     * @return Named node count.
     */
    [[nodiscard]] size_t GetNamedNodeCount() const noexcept
    {
        return m_namedNodeCount;
    }

    /**
     * @brief Checks if the budget has been exceeded without throwing.
     * @return True if visits exceed maximum allowed.
     */
    [[nodiscard]] bool IsExceeded() const noexcept
    {
        return m_maxAllowed > 0 && m_currentVisits > m_maxAllowed;
    }

    /**
     * @brief Resets current visits to zero while keeping the budget ceiling.
     */
    void Reset() noexcept
    {
        m_currentVisits = 0;
    }

    /**
     * @brief Configures budget ceiling from a new AST root node.
     * @param[in] root The AST root node.
     * @param[in] multiplier Visit multiplier relative to named nodes.
     */
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
