#pragma once

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/ValueExpressionFacts.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Sentinel indicating an initializer list is top-level or an element is not a nested list.
 */
inline constexpr uint32_t NoInitializerList = UINT32_MAX;

/**
 * @brief Syntactic classification of an initializer list element expression.
 */
enum class InitializerListElementKind : uint8_t
{
    Other,      ///< Unclassified or composite expression (e.g. arithmetic, ternary, index).
    NestedList, ///< Nested initializer list `{ ... }`.
    Literal,    ///< Numeric, string, boolean, or null literal.
    Identifier, ///< Variable, parameter, or scoped identifier.
    Call        ///< Function call or construct call expression.
};

/**
 * @brief One element expression inside an initializer list with precomputed value and operand facts.
 */
struct InitializerListElementFact
{
    IndexedByteRange bytes;                                              ///< Byte span of the element expression.
    SourceRange range;                                                   ///< Line and column span used for diagnostics.
    std::string_view text;                                               ///< Element source text from the document.
    InitializerListElementKind kind = InitializerListElementKind::Other; ///< Syntactic kind of this element.
    uint32_t nestedListIndex =
        NoInitializerList;     ///< Index in InitializerListFacts::lists when kind == NestedList, or NoInitializerList.
    ValueExpressionFact value; ///< Value expression fact; input of ResolveValueType in pass 2.
    BinaryOperandFact operand; ///< Binary operand fact; anchors diagnostics and operand flags.
};

/**
 * @brief Kind of declaration, statement, or expression that encloses an initializer list.
 */
enum class InitializerListOwnerKind : uint8_t
{
    None,                ///< Standalone list or unclassified owner context.
    VariableDeclaration, ///< Variable declaration (`Type a = { ... };`).
    TypedList,           ///< Typed initializer list or anonymous object (`Type = { ... }` or `Type { ... }`).
    Assignment,          ///< Right-hand side of an assignment (`a = { ... };`).
    ReturnStatement,     ///< Returned value in a return statement (`return { ... };`).
    CallArgument,        ///< Argument in a function or construct call (`func({ ... })`).
    ParentList           ///< Nested inside an enclosing initializer list (`{ { ... } }`).
};

/**
 * @brief Context of the owning declaration, statement, or call expression enclosing an initializer list.
 */
struct InitializerListOwnerFact
{
    InitializerListOwnerKind kind = InitializerListOwnerKind::None; ///< Owning syntax site kind.
    std::string_view targetType; ///< Syntactically available target type spelling (from declaration, typed list, return
                                 ///< statement, or construct call).
    IndexedByteRange ownerBytes; ///< Byte span of the owning declaration, statement, or call expression.
    SourceRange ownerRange;      ///< Line and column span of the owning site; start selects lexical scope.
    IndexedByteRange targetBytes; ///< Byte span of assignment target / LHS expression; empty when not an assignment.
    IndexedByteRange
        subscriptObjectBytes; ///< Byte span of index expression object if target is a subscript; empty otherwise.
    bool isSubscriptAssignment = false; ///< True when assigned to an index expression (`a[i] = { ... }`).
    bool hasStringIndex = false;        ///< True when the index expression uses a string literal index.
    uint32_t argumentIndex = 0;         ///< 0-based argument position when owner is CallArgument.
};

/**
 * @brief An initializer list syntax node with flattened element slices, nesting links, and owner context.
 */
struct InitializerListFact
{
    IndexedByteRange bytes; ///< Byte span of the `{ ... }` initializer list.
    SourceRange range;      ///< Line and column span of the list node.
    uint32_t parentListIndex =
        NoInitializerList;          ///< Index of enclosing list in InitializerListFacts::lists, or NoInitializerList.
    uint32_t elementBegin = 0;      ///< First slice index into InitializerListFacts::elements.
    uint32_t elementCount = 0;      ///< Number of direct child elements in this list.
    uint32_t valueCount = 0;        ///< Value count (commas + 1 when commas > 0, else 1 if elements > 0, else 0).
    InitializerListOwnerFact owner; ///< Context of the owning declaration, assignment, return or call.
};

/**
 * @brief AST-free initializer list and element facts of one syntax revision.
 * @note Text views point into the source passed to BuildInitializerListFacts, which must outlive this object.
 */
struct InitializerListFacts
{
    std::vector<InitializerListFact> lists;           ///< Initializer lists ordered by span (pre-order).
    std::vector<InitializerListElementFact> elements; ///< Flat vector of elements, sliced by InitializerListFact.

    /**
     * @brief Finds the initializer list that spans exactly the given bytes.
     * @param[in] bytes Byte span of the list node.
     * @return Matching fact, or nullptr when the node was not indexed.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const InitializerListFact* Find(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(lists, bytes);
    }

    /**
     * @brief Finds the initializer list that spans exactly the given bytes.
     * @param[in] bytes Byte span of the list node.
     * @return Matching fact, or nullptr when the node was not indexed.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const InitializerListFact* FindList(IndexedByteRange bytes) const noexcept
    {
        return Find(bytes);
    }

    /**
     * @brief Returns a span over the element facts for the given initializer list.
     * @param[in] list Initializer list fact.
     * @return Span of elements belonging to the list.
     */
    [[nodiscard]] std::span<const InitializerListElementFact> Elements(const InitializerListFact& list) const noexcept
    {
        if (list.elementBegin >= elements.size() || list.elementCount == 0)
        {
            return {};
        }
        return std::span<const InitializerListElementFact>(elements.data() + list.elementBegin, list.elementCount);
    }
};

/**
 * @brief Extracts all initializer lists, nested elements, and owner contexts of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; facts contain views into this buffer.
 * @return Initializer list facts with lists ordered by start byte ascending, end byte descending.
 */
[[nodiscard]] InitializerListFacts BuildInitializerListFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
