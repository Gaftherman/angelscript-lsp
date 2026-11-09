#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Syntactic shape of an expression that may name a virtual property.
 */
enum class PropertyAccessKind : uint8_t
{
    None,  ///< The expression cannot name a property.
    Name,  ///< Identifier or scoped identifier.
    Member ///< Member expression whose receiver and member were both found.
};

/**
 * @brief Spelling and spans of an expression that may name a virtual property.
 */
struct PropertyAccessFact
{
    PropertyAccessKind kind = PropertyAccessKind::None;
    IndexedByteRange bytes;    ///< Byte span of the expression; positions scope and container lookups.
    uint16_t nodeSymbol = 0;   ///< Grammar symbol of the expression; selects the container lookup context.
    std::string_view text;     ///< Name: trimmed spelling. Member: spelling of the member token.
    IndexedByteRange receiver; ///< Member only: byte span of the receiver expression.
};

/**
 * @brief Captures the spans and spelling that property accessor inspection reads from an expression.
 * @param[in] node Expression node; a null node yields a fact of kind None.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; the fact's views point into it.
 * @return Property access fact for the node.
 */
[[nodiscard]] PropertyAccessFact ReadPropertyAccess(TSNode node, const NodeIndex* index, std::string_view source);

} // namespace angel_lsp::analysis
