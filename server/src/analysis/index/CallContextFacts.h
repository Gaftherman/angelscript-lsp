#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <optional>

namespace angel_lsp::analysis
{

/**
 * @brief Classification of function or constructor invocation syntax.
 */
enum class CallContextKind : uint8_t
{
    Direct,                ///< Direct unqualified call or constructor invocation.
    Member,                ///< Member method invocation via object access.
    Qualified,             ///< Scoped or namespace-qualified call.
    ConstructorDeclaration ///< Constructor call within a variable declarator.
};

/**
 * @brief Extracted coordinate facts and parameter offset for a call context.
 */
struct IndexedCallContext
{
    uint32_t calleeStartByte = 0;    ///< Inclusive start byte offset of the callee identifier.
    uint32_t calleeEndByte = 0;      ///< Exclusive end byte offset of the callee identifier.
    uint32_t receiverStartByte = 0;  ///< Member: object expression start. Qualified: `A::B` qualifier start.
    uint32_t receiverEndByte = 0;    ///< Exclusive end of the receiver; empty span for a global `::f` call.
    uint32_t prevTokenStartByte = 0; ///< ConstructorDeclaration: start of the declared type in `T name(`.
    uint32_t prevTokenEndByte = 0;   ///< ConstructorDeclaration: exclusive end of the declared type.
    uint32_t activeParameter = 0;    ///< Zero-based index of active parameter based on comma separators.
    CallContextKind kind = CallContextKind::Direct; ///< Classification of call expression syntax.
};

/**
 * @brief Discovers the active call context enclosing cursorByte.
 *
 * Uses the innermost argument_list around the cursor; while a call is still typed, it falls back to the last
 * unclosed `(` of the innermost ERROR node that reaches the cursor.
 *
 * Cost: O(A + E) over the indexed argument_list and ERROR nodes that start before the cursor, plus O(k) over the
 * tokens of the chosen container.
 *
 * @param[in] index Document-wide node index.
 * @param[in] cursorByte Target cursor byte offset in document source.
 * @return Extracted call context facts, or std::nullopt if cursor is not inside a callable invocation.
 */
std::optional<IndexedCallContext> FindCallContext(const NodeIndex& index, uint32_t cursorByte);

} // namespace angel_lsp::analysis
