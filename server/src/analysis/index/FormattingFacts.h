#pragma once

#include "analysis/index/NodeIndex.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Byte layout of one top-level declaration in a predefined stub, captured in the first pass.
 * @note Comments at the top level are not recorded; the text between two declarations stays in the source.
 */
struct IndexedStubDeclaration
{
    uint32_t startByte = 0;      ///< First byte of the declaration.
    uint32_t endByte = 0;        ///< One past the last byte of the declaration.
    uint32_t nameStartByte = 0;  ///< First byte of the namespace name; 0 unless `hasNameAndBody`.
    uint32_t nameEndByte = 0;    ///< One past the namespace name; 0 unless `hasNameAndBody`.
    uint32_t bodyStartByte = 0;  ///< Byte of the opening `{` of the namespace body; 0 unless `hasNameAndBody`.
    uint32_t bodyEndByte = 0;    ///< One past the closing `}` of the namespace body; 0 unless `hasNameAndBody`.
    bool isNamespace = false;    ///< True for a `namespace_declaration`.
    bool hasNameAndBody = false; ///< True when a namespace has both its name and its body fields.
};

/**
 * @brief Tells whether on-type formatting must stay away from the given position.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] line 0-based line of the typed character.
 * @param[in] column 0-based column of the typed character.
 * @return True when the tree has a syntax error anywhere or the position lies inside a string literal or a
 *         comment; false otherwise, including when the index has no tree.
 * @note Cost: one O(depth) descent to the position plus a bounded ancestor climb.
 */
[[nodiscard]] bool IsOnTypeFormattingSuppressed(const NodeIndex& index, uint32_t line, uint32_t column);

/**
 * @brief Parses a predefined stub and lists its top-level declarations in source order.
 * @param[in] source Stub text.
 * @param[in,out] parser Parser used for this one parse; the tree is released before returning.
 * @return Declarations without top-level comments, or nullopt when the parse fails or the tree has an error.
 * @note Cost: one parse plus one flat cursor pass over the root's children.
 */
[[nodiscard]] std::optional<std::vector<IndexedStubDeclaration>>
ExtractStubDeclarations(std::string_view source, parser::AngelScriptParser& parser);

} // namespace angel_lsp::analysis
