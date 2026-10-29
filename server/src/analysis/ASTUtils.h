#pragma once

#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
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
using parser::NodeType;
using parser::UnquoteStringView;

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
    parser::ForEachDescendantNode(root,
                                  [&](TSNode current)
                                  {
                                      if (ts_node_is_named(current))
                                      {
                                          ++count;
                                      }
                                  });
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
