#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Zero-based indices corresponding to the server semantic tokens legend types.
 */
enum TokenTypeIndex : uint32_t
{
    Type_Namespace = 0,
    Type_Type = 1,
    Type_Class = 2,
    Type_Enum = 3,
    Type_Interface = 4,
    Type_Struct = 5,
    Type_TypeParameter = 6,
    Type_Parameter = 7,
    Type_Variable = 8,
    Type_Property = 9,
    Type_EnumMember = 10,
    Type_Event = 11,
    Type_Function = 12,
    Type_Method = 13,
    Type_Macro = 14,
    Type_Keyword = 15,
    Type_Modifier = 16,
    Type_Comment = 17,
    Type_String = 18,
    Type_Number = 19,
    Type_Regexp = 20,
    Type_Operator = 21,
    Type_Decorator = 22,
    Type_TemplatePunctuation = 23
};

/**
 * @brief Bit flags matching the server semantic token modifiers legend.
 */
enum TokenModifierBit : uint32_t
{
    Mod_Declaration = 1 << 0,
    Mod_Definition = 1 << 1,
    Mod_Readonly = 1 << 2,
    Mod_Static = 1 << 3,
    Mod_Deprecated = 1 << 4,
    Mod_Abstract = 1 << 5,
    Mod_Async = 1 << 6,
    Mod_Modification = 1 << 7,
    Mod_Documentation = 1 << 8,
    Mod_DefaultLibrary = 1 << 9
};

/**
 * @brief AST-free pre-extracted syntax highlight capture record.
 */
struct IndexedHighlightCapture
{
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    uint32_t startLine = 0;
    uint32_t startColumn = 0;
    uint32_t endLine = 0;
    uint32_t endColumn = 0;
    uint32_t tokenType = 0;
    uint32_t tokenModifiers = 0;
    int priority = 0;
    bool isTemplatePunctuationNode = false;
    bool isBaseListScopedLeaf = false;
    uint32_t qualifierStartByte = 0;
    uint32_t qualifierEndByte = 0;
};

/**
 * @brief Byte ranges of components in a scoped_identifier node.
 */
struct IndexedScopedName
{
    uint32_t leftStartByte = 0;
    uint32_t leftEndByte = 0;
    uint32_t prevStartByte = 0;
    uint32_t prevEndByte = 0;
    uint32_t rightStartByte = 0;
    uint32_t rightEndByte = 0;
    uint32_t nodeStartByte = 0;
    uint32_t nodeEndByte = 0;
};

/**
 * @brief Byte ranges of components in a scoped type node.
 */
struct IndexedScopedType
{
    uint32_t scopeTextStartByte = 0;
    uint32_t scopeTextEndByte = 0;
    uint32_t memberTextStartByte = 0;
    uint32_t memberTextEndByte = 0;
    uint32_t scopeUpgradeByte = 0;
    uint32_t memberUpgradeByte = 0;
};

/**
 * @brief Precomputed byte interval and qualified name for a class declaration span.
 */
struct IndexedClassSpan
{
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    std::string qualifiedName;
    int32_t parentIndex = -1;
};

/**
 * @brief Immutable collection of Pass 1 syntax facts for semantic token computation.
 */
struct SemanticTokenFacts
{
    std::vector<IndexedHighlightCapture> captures;
    std::vector<IndexedScopedName> scopedNames;
    std::vector<IndexedScopedType> scopedTypes;
    std::vector<IndexedClassSpan> classSpans;

    /**
     * @brief Finds the qualified name of the innermost enclosing class declaration.
     * @param[in] byteOffset Target document byte position.
     * @return Enclosing class qualified name view, or empty if outside classes.
     */
    [[nodiscard]] std::string_view FindEnclosingClass(uint32_t byteOffset) const;
};

/**
 * @brief Checks whether the given text is punctuation or bracket that must never be an operator.
 * @param[in] text Token text.
 * @return True if token is punctuation or bracket.
 */
[[nodiscard]] bool IsPunctuationOrBracket(std::string_view text) noexcept;

/**
 * @brief Checks whether the given text is a genuine AngelScript operator token.
 * @param[in] text Token text.
 * @return True if token is genuine operator.
 */
[[nodiscard]] bool IsGenuineOperator(std::string_view text) noexcept;

/**
 * @brief Extracts AST-free syntax facts for semantic token highlighting from the syntax tree.
 * @param[in] index Pre-indexed syntax tree facts.
 * @param[in] sourceCode Document source text buffer.
 * @return Populated SemanticTokenFacts collection.
 */
[[nodiscard]] SemanticTokenFacts CollectSemanticTokenFacts(const NodeIndex& index, std::string_view sourceCode);

} // namespace angel_lsp::analysis
