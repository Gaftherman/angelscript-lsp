#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::parser
{

/**
 * @brief Categorization of syntax nodes produced by the Doxygen comment grammar.
 */
enum class DoxygenNodeKind : uint8_t
{
    Root,
    BriefHeader,
    BriefDescription,
    Description,
    CodeBlock,
    CodeBlockLanguage,
    CodeBlockContent,
    Tag,
    TagName,
    StorageClass,
    Identifier,
    QualifiedIdentifier,
    Function,
    FunctionLink,
    Type,
    TextLine,
    CodeWord,
    Code,
    Emphasis,
    Text,
    Link,
    Other
};

/**
 * @brief Compact, AST-free snapshot of a single Doxygen parse tree node.
 *        Direct child nodes occupy the contiguous range nodes[firstChild, firstChild + childCount).
 */
struct DoxygenNodeFact
{
    /**
     * @brief Semantic kind identifier mapping to Doxygen grammar constructs.
     */
    DoxygenNodeKind kind = DoxygenNodeKind::Other;

    /**
     * @brief True if the tree-sitter node was named in the grammar.
     */
    bool named = false;

    /**
     * @brief Inclusive starting byte offset within the parsed comment source.
     */
    uint32_t startByte = 0;

    /**
     * @brief Exclusive ending byte offset within the parsed comment source.
     */
    uint32_t endByte = 0;

    /**
     * @brief Offset in the facts node buffer where this node's direct children begin.
     */
    uint32_t firstChild = 0;

    /**
     * @brief Number of direct child nodes stored contiguously from firstChild.
     */
    uint32_t childCount = 0;
};

/**
 * @brief Flattened collection of Doxygen syntax node facts extracted in first pass.
 *        When non-empty, index 0 is always the root node.
 */
struct DoxygenSyntaxFacts
{
    /**
     * @brief Contiguous buffer of all syntax node records in breadth-first sequence.
     */
    std::vector<DoxygenNodeFact> nodes;
};

/**
 * @brief Lightweight, non-owning view over an indexed Doxygen syntax fact node.
 * @warning A view must not outlive the facts object nor the source text it was built from.
 */
class DoxygenNodeView
{
  private:
    /**
     * @brief Pointer to the immutable facts table owning the node data.
     */
    const DoxygenSyntaxFacts* m_facts = nullptr;

    /**
     * @brief Index of the targeted node inside the facts nodes vector.
     */
    uint32_t m_index = 0;

    /**
     * @brief Non-owning view of the original comment source buffer.
     */
    std::string_view m_source;

  public:
    /**
     * @brief Constructs an uninitialized, null node view.
     */
    DoxygenNodeView() = default;

    /**
     * @brief Constructs a view over a specific indexed node within Doxygen syntax facts.
     * @param[in] facts Reference to the immutable parsed facts table.
     * @param[in] index Index of the target node within the facts nodes buffer.
     * @param[in] source Original source text slice corresponding to the parsed comment.
     */
    DoxygenNodeView(const DoxygenSyntaxFacts& facts, uint32_t index, std::string_view source) noexcept
        : m_facts(&facts), m_index(index), m_source(source)
    {
    }

    /**
     * @brief Creates a node view representing the root node of the syntax facts.
     * @param[in] facts Reference to the immutable parsed facts table.
     * @param[in] source Original source text slice corresponding to the parsed comment.
     * @return A node view for the root node, or a null view if the facts container is empty.
     */
    [[nodiscard]] static DoxygenNodeView Root(const DoxygenSyntaxFacts& facts, std::string_view source) noexcept
    {
        if (facts.nodes.empty())
        {
            return DoxygenNodeView();
        }
        return DoxygenNodeView(facts, 0, source);
    }

    /**
     * @brief Checks whether this view references a valid node in the facts table.
     * @return True if m_facts is null or m_index is out of range; false otherwise.
     */
    [[nodiscard]] bool IsNull() const noexcept
    {
        return m_facts == nullptr || m_index >= m_facts->nodes.size();
    }

    /**
     * @brief Retrieves the grammatical kind of the referenced node.
     * @return Node kind enum value, or DoxygenNodeKind::Other if null.
     */
    [[nodiscard]] DoxygenNodeKind Kind() const noexcept
    {
        if (IsNull())
        {
            return DoxygenNodeKind::Other;
        }
        return m_facts->nodes[m_index].kind;
    }

    /**
     * @brief Returns the inclusive start byte offset of the node in the source text.
     * @return 0-based start byte index, or 0 if null.
     */
    [[nodiscard]] uint32_t StartByte() const noexcept
    {
        if (IsNull())
        {
            return 0;
        }
        return m_facts->nodes[m_index].startByte;
    }

    /**
     * @brief Returns the exclusive end byte offset of the node in the source text.
     * @return 0-based end byte index, or 0 if null.
     */
    [[nodiscard]] uint32_t EndByte() const noexcept
    {
        if (IsNull())
        {
            return 0;
        }
        return m_facts->nodes[m_index].endByte;
    }

    /**
     * @brief Extracts the source text slice covered by this syntax node.
     * @return Substring view of the source buffer, or an empty view if invalid or null.
     */
    [[nodiscard]] std::string_view Text() const noexcept
    {
        if (IsNull())
        {
            return std::string_view();
        }
        const uint32_t start = m_facts->nodes[m_index].startByte;
        const uint32_t end = m_facts->nodes[m_index].endByte;
        if (start >= end || end > m_source.size())
        {
            return std::string_view();
        }
        return m_source.substr(start, end - start);
    }

    /**
     * @brief Returns the full source text buffer associated with this view.
     * @return Original source text view, or empty view if null.
     */
    [[nodiscard]] std::string_view Source() const noexcept
    {
        if (IsNull())
        {
            return std::string_view();
        }
        return m_source;
    }

    /**
     * @brief Returns the count of direct child nodes.
     * @return Number of direct children, or 0 if null.
     */
    [[nodiscard]] uint32_t ChildCount() const noexcept
    {
        if (IsNull())
        {
            return 0;
        }
        return m_facts->nodes[m_index].childCount;
    }

    /**
     * @brief Retrieves the i-th direct child of this node.
     * @param[in] i 0-based index within this node's direct children.
     * @return View of the requested child node, or a null view if i is out of range.
     */
    [[nodiscard]] DoxygenNodeView Child(uint32_t i) const noexcept
    {
        if (IsNull() || i >= m_facts->nodes[m_index].childCount)
        {
            return DoxygenNodeView();
        }
        const uint32_t childIndex = m_facts->nodes[m_index].firstChild + i;
        if (childIndex >= m_facts->nodes.size())
        {
            return DoxygenNodeView();
        }
        return DoxygenNodeView(*m_facts, childIndex, m_source);
    }

    /**
     * @brief Counts how many direct children have the named flag set to true.
     * @return Number of named direct child nodes, or 0 if null.
     */
    [[nodiscard]] uint32_t NamedChildCount() const noexcept
    {
        if (IsNull())
        {
            return 0;
        }
        const auto& node = m_facts->nodes[m_index];
        const uint32_t endChild = node.firstChild + node.childCount;
        if (endChild > m_facts->nodes.size())
        {
            return 0;
        }
        uint32_t count = 0;
        for (uint32_t idx = node.firstChild; idx < endChild; ++idx)
        {
            if (m_facts->nodes[idx].named)
            {
                ++count;
            }
        }
        return count;
    }

    /**
     * @brief Retrieves the i-th direct child whose named flag is true.
     * @param[in] i 0-based index among named children.
     * @return View of the i-th named child, or a null view if no such child exists.
     */
    [[nodiscard]] DoxygenNodeView NamedChild(uint32_t i) const noexcept
    {
        if (IsNull())
        {
            return DoxygenNodeView();
        }
        const auto& node = m_facts->nodes[m_index];
        const uint32_t endChild = node.firstChild + node.childCount;
        if (endChild > m_facts->nodes.size())
        {
            return DoxygenNodeView();
        }
        uint32_t currentNamed = 0;
        for (uint32_t idx = node.firstChild; idx < endChild; ++idx)
        {
            if (m_facts->nodes[idx].named)
            {
                if (currentNamed == i)
                {
                    return DoxygenNodeView(*m_facts, idx, m_source);
                }
                ++currentNamed;
            }
        }
        return DoxygenNodeView();
    }

    /**
     * @brief Searches direct children for the first node matching the specified kind.
     * @param[in] kind The node kind to look for.
     * @return View of the first matching direct child, or a null view if not found.
     */
    [[nodiscard]] DoxygenNodeView FindChild(DoxygenNodeKind kind) const noexcept
    {
        if (IsNull())
        {
            return DoxygenNodeView();
        }
        const auto& node = m_facts->nodes[m_index];
        const uint32_t endChild = node.firstChild + node.childCount;
        if (endChild > m_facts->nodes.size())
        {
            return DoxygenNodeView();
        }
        for (uint32_t idx = node.firstChild; idx < endChild; ++idx)
        {
            if (m_facts->nodes[idx].kind == kind)
            {
                return DoxygenNodeView(*m_facts, idx, m_source);
            }
        }
        return DoxygenNodeView();
    }
};

/**
 * @brief Parses Doxygen doc comment source text into an immutable snapshot of node facts.
 * @param[in] text Comment source slice to parse.
 * @return Extracted syntax facts containing all parsed nodes in breadth-first order.
 */
[[nodiscard]] DoxygenSyntaxFacts ExtractDoxygenSyntax(std::string_view text);

} // namespace angel_lsp::parser
