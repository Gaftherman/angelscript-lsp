#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Preference ranking for resolving symbols within enclosing container scopes versus imported scopes.
 */
enum class ContainerContextPreference : uint8_t
{
    None = 0,
    PreferClass,
    PreferNamespace
};

/**
 * @brief Primary syntactic classification of the hover target node.
 */
enum class HoverNodeKind : uint8_t
{
    None = 0,
    Identifier,
    ScopedMember,
    MemberAccess,
    Parameter,
    NamedArgument,
    EnumMember,
    This,
    Lambda,
    Auto,
    StringLiteral,      ///< Quoted `string_literal` target (it may still sit inside a `+` chain).
    ConcatenatedString, ///< Quoted `binary_expression` target, the whole concatenation.
    PrimitiveOrType
};

/**
 * @brief AST-free hover site descriptor capturing target geometry, kind, and structural syntactic contexts.
 */
struct IndexedHoverSite
{
    uint32_t targetStartByte = 0;                   ///< Byte offset where target token starts.
    uint32_t targetEndByte = 0;                     ///< Byte offset where target token ends.
    uint32_t startLine = 0;                         ///< Zero-based line index of target start.
    uint32_t startColumn = 0;                       ///< Zero-based column offset of target start.
    uint32_t endLine = 0;                           ///< Zero-based line index of target end.
    uint32_t endColumn = 0;                         ///< Zero-based column offset of target end.
    HoverNodeKind kind = HoverNodeKind::None;       ///< Primary syntax classification of target.
    HoverNodeKind parentKind = HoverNodeKind::None; ///< Syntax classification of parent AST node.
    uint32_t parentStartByte = 0;                   ///< Byte offset where parent AST node starts.
    uint32_t parentEndByte = 0;                     ///< Byte offset where parent AST node ends.
    IndexedByteRange receiverRange{};               ///< Byte range of receiver object expression.
    IndexedByteRange enclosingCallRange{};          ///< Byte range of enclosing call expression.
    IndexedByteRange argumentListRange{};           ///< Byte range of argument list of call or declarator.
    IndexedByteRange directInitDeclaratorRange{};   ///< Byte range of direct-init declarator.
    IndexedByteRange enclosingLambdaRange{};        ///< Byte range of enclosing lambda expression.
    IndexedByteRange enclosingAutoDeclRange{};      ///< Byte range of enclosing auto declaration.
    IndexedByteRange topmostBinaryPlusRange{};      ///< Byte range of topmost string binary plus expression.
    IndexedByteRange scopeQualifierRange{};         ///< Parent `scoped_identifier` start through target end.
    IndexedByteRange datatypeScopeRange{};          ///< Non-empty `scope` child of the type holding a datatype.
    IndexedByteRange namedArgCallRange{};           ///< Grandparent of a named-argument label (the callee node).
    IndexedByteRange nextScopedMemberRange{};       ///< Byte range of next member in scoped identifier.
    IndexedByteRange enclosingEnumNameRange{};      ///< Byte range of enclosing enum declaration name.
    ContainerContextPreference contextPreference = ContainerContextPreference::None; ///< Context resolution preference.
    uint8_t isScopeQualifier : 1 = 0;       ///< True if target qualifies next member via ::.
    uint8_t isQualifiedMember : 1 = 0;      ///< True if target is qualified member or scope leaf.
    uint8_t isMemberOfExpression : 1 = 0;   ///< True if target is member of member_expression.
    uint8_t isNamedArgumentLabel : 1 = 0;   ///< True if target is label before colon in call.
    uint8_t isDirectInitDeclarator : 1 = 0; ///< True if target is direct-init declarator.
    uint8_t isGlobalScopeQualified : 1 = 0; ///< True if target has leading global scope ::.
    uint8_t isDirectRootGlobal : 1 = 0;     ///< True if target is direct root member under leading ::.
    uint8_t isAutoHandle : 1 = 0;           ///< True if target auto keyword specifies handle.
    uint8_t isParameterName : 1 = 0;        ///< True if target is the `name` field of its parent parameter.
};

/**
 * @brief Parsed include directive metadata recording column anchors and raw path byte bounds.
 */
struct IndexedHoverIncludeDirective
{
    size_t hash = 0;            ///< Column index of the hash symbol on the line.
    size_t closeQuote = 0;      ///< Column index of the closing delimiter on the line.
    uint32_t pathStartByte = 0; ///< Absolute byte offset of the path text start.
    uint32_t pathEndByte = 0;   ///< Absolute byte offset of the path text end.
};

/**
 * @brief Locates the hover site under the given line and column, capturing AST facts into an AST-free descriptor.
 * @param[in] index Document NodeIndex retaining the parsed Tree-Sitter AST root.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] line Zero-based document line index.
 * @param[in] column Zero-based document column offset.
 * @return Populated IndexedHoverSite if a hoverable symbol or literal resides at or adjacent to position, std::nullopt
 * otherwise.
 * @note Traversal cost consists of one descent O(depth) to locate point descendant, plus bounded ancestor searches
 * (depth <= 8 hops) and bounded sibling scans (<= 8 hops).
 */
[[nodiscard]] std::optional<IndexedHoverSite> FindHoverSite(const NodeIndex& index, std::string_view sourceCode,
                                                            uint32_t line, uint32_t column);

/**
 * @brief Parses an include directive at the target line, validating delimiter spans.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] line Zero-based document line index.
 * @param[in] column Optional zero-based column offset to verify point containment on directive line.
 * @return Populated IndexedHoverIncludeDirective if the line contains an include directive spanning the column,
 * std::nullopt otherwise.
 * @note Cost is O(L) where L is the length of line text; performs linear whitespace and delimiter scans without heap
 * allocations.
 */
[[nodiscard]] std::optional<IndexedHoverIncludeDirective>
FindIncludeDirective(std::string_view sourceCode, uint32_t line, std::optional<uint32_t> column = std::nullopt);
} // namespace angel_lsp::analysis
